/** aggressor: HTTP connection
2022, Simon Zolin */

#include <aggressor.h>
#include <util/http1.h>
#include <ffbase/atomic.h>

static void conn_connect(struct conn *c);
static void conn_req_send(struct conn *c);
static void conn_resp_recv(struct conn *c);
static int conn_resp_parse(struct conn *c);
static void conn_respdata_recv(struct conn *c);
static void conn_end(struct conn *c);

static void conn_attach(struct conn *c)
{
	if (!c->kq_attach_ok) {
		c->kq_attach_ok = 1;
		if (0 != ffkq_attach_socket(c->w->kq, c->sk, (void*)((ffsize)c | c->side), FFKQ_READWRITE))
			agg_syserr("%p: ffkq_attach_socket", c);
		agg_dbg("%p: kq attached", c);
	}
}

static void conn_prep(struct conn *c)
{
	ffmem_zero(&c->wdata, sizeof(struct conn) - FF_OFF(struct conn, wdata));
}

void conn_start(struct conn *c, struct worker *w)
{
	ffmem_zero(c, FF_OFF(struct conn, wdata));
	conn_prep(c);
	c->w = w;

	c->sk = ffsock_create_tcp(agg_conf->addr.ip4.sin_family, FFSOCK_NONBLOCK);
	if (c->sk == FFSOCK_NULL) {
		agg_syserr("sock create");
		c->w->stats.connections_failed++;
		conn_end(c);
		return;
	}
	agg_dbg("%p: new connection", c);

#ifdef FF_WIN
	conn_attach(c);
#endif

	conn_connect(c);
}

static void conn_connect(struct conn *c)
{
	if (c->whandler == NULL) {
		c->start_time_usec = time_usec();
	} else {
		c->whandler = NULL;
	}
	if (0 != ffsock_connect_async(c->sk, &agg_conf->addr, &c->kqtask)) {
		if (fferr_last() != FFSOCK_EINPROGRESS) {
			agg_syserr("sock connect");
			c->w->stats.connections_failed++;
			conn_end(c);
			return;
		}
		agg_dbg("%p: connecting", c);
		conn_attach(c);
		c->whandler = conn_connect;
		return;
	}

	c->w->stats.connections_ok++;

	agg_dbg("%p: connected", c);

	ffuint64 t = time_usec();
	c->w->stats.connect_latency_usec = (c->w->stats.connect_latency_usec + t - c->start_time_usec) / 2;
	conn_req_send(c);
}

static void conn_req_send(struct conn *c)
{
	if (c->wdata.len == 0) {
		c->wdata = *ffslice_itemT(&agg_conf->reqs, c->w->next_req, ffstr);
		c->w->next_req++;
		if (c->w->next_req == agg_conf->reqs.len)
			c->w->next_req = 0;
		if (0 != ffsock_setopt(c->sk, IPPROTO_TCP, TCP_NODELAY, 1))
			agg_syserr("set TCP_NODELAY");
	}

	while (c->wdata.len != 0) {
		int r = ffsock_send_async(c->sk, c->wdata.ptr, c->wdata.len, &c->kqtask2);
		if (r < 0) {
			if (fferr_last() != FFSOCK_EINPROGRESS) {
				agg_syserr("sock send");
				conn_end(c);
				return;
			}
			agg_dbg("%p: sending request", c);
			conn_attach(c);
			c->whandler = conn_req_send;
			return;
		}

		ffstr_shift(&c->wdata, r);
		c->w->stats.total_sent += r;
	}

	agg_dbg("%p: sent request", c);

	c->start_time_usec = time_usec();
	conn_resp_recv(c);
}

static void conn_resp_recv(struct conn *c)
{
	for (;;) {
		int r = ffsock_recv_async(c->sk, c->buf + c->bufn, agg_conf->rbuf_size - c->bufn, &c->kqtask);
		if (r < 0) {
			if (fferr_last() != FFSOCK_EINPROGRESS) {
				agg_syserr("sock recv");
				break;
			}
			agg_dbg("%p: receiving response", c);
			conn_attach(c);
			c->rhandler = conn_resp_recv;
			return;
		} else if (r == 0) {
			agg_err("server closed connection");
			break;
		}

		c->bufn += r;
		c->w->stats.total_recv += r;

		agg_dbg("%p: response receive +%L", c, r);

		r = conn_resp_parse(c);
		if (r < 0) {
			break;
		} else if (r == 0) {
			return;
		}

		if (c->bufn == agg_conf->rbuf_size) {
			agg_err("too large HTTP response");
			break;
		}
	}

	conn_end(c);
}

static int conn_resp_parse(struct conn *c)
{
	ffstr resp = FFSTR_INITN(c->buf, c->bufn), proto, msg;
	uint code;
	int r = http_resp_parse(resp, &proto, &code, &msg);
	if (r < 0) {
		agg_err("bad HTTP response line");
		return -1;
	} else if (r == 0) {
		return 1;
	}
	ffstr_shift(&resp, r);

	if (!c->resp_line_ok) {
		c->resp_line_ok = 1;
		ffuint64 t = time_usec();
		c->w->stats.resp_latency_usec = (c->w->stats.resp_latency_usec + t - c->start_time_usec) / 2;
	}

	ffstr name = {}, val = {};
	for (;;) {
		r = http_hdr_parse(resp, &name, &val);
		if (r == 0) {
			return 1;
		} else if (r < 0) {
			agg_err("bad HTTP header");
			return -1;
		}
		ffstr_shift(&resp, r);

		if (r <= 2)
			break;

		if (ffstr_ieqz(&name, "Content-Length")) {
			if (!ffstr_to_uint64(&val, &c->cont_len)) {
				agg_err("bad Content-Length");
				return -1;
			}
		}
	}

	if (resp.len > c->cont_len) {
		agg_err("received data %L is larger than Content-Length %U"
			, resp.len, c->cont_len);
		return -1;
	}
	c->cont_len -= resp.len;

	if (code/100 == 4 || code/100 == 5)
		c->resp_err = 1;

	conn_respdata_recv(c);
	return 0;
}

static void conn_respdata_recv(struct conn *c)
{
	while (c->cont_len != 0) {
		uint n = ffmin(c->cont_len, agg_conf->rbuf_size);
		int r = ffsock_recv_async(c->sk, c->buf, n, &c->kqtask);
		if (r < 0) {
			if (fferr_last() != FFSOCK_EINPROGRESS) {
				agg_syserr("sock recv");
				goto end;
			}
			conn_attach(c);
			c->rhandler = conn_respdata_recv;
			return;
		} else if (r == 0) {
			agg_err("server closed connection");
			goto end;
		}

		c->cont_len -= r;
		c->w->stats.total_recv += r;
	}

	if (c->resp_err)
		c->w->stats.resp_err++;
	else
		c->w->stats.resp_ok++;

	agg_dbg("%p: response finished", c);

	c->keepalive++;
	if (c->keepalive == agg_conf->keepalive_reqs)
		goto end;

	if (agg_conn_fin(c, 0))
		return;

	conn_prep(c);
	conn_req_send(c);
	return;

end:
	conn_end(c);
}

void conn_close(struct conn *c)
{
	ffsock_close(c->sk);  c->sk = FFSOCK_NULL;
}

static void conn_end(struct conn *c)
{
	conn_close(c);
	c->side = !c->side;
	agg_dbg("connection finished");
	agg_conn_fin(c, 1);
}
