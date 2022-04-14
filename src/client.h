/** aggressor: HTTP connection
2022, Simon Zolin */

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
		ffkq_attach_socket(c->w->kq, c->sk, (void*)((ffsize)c | c->side), FFKQ_READWRITE);
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

	int n = ffint_fetch_add(&agg_conf->total_reqs, -1);
	if (n < 0) {
		return;
	} else if (n == 0) {
		c->last = 1;
	}

	c->sk = ffsock_create_tcp(agg_conf->addr.ip4.sin_family, FFSOCK_NONBLOCK);
	if (c->sk == FFSOCK_NULL) {
		agg_syserr("sock create");
		c->w->stats.connections_failed++;
		return;
	}
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
		conn_attach(c);
		c->whandler = conn_connect;
		return;
	}

	c->w->stats.connections_ok++;

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
		ffsock_setopt(c->sk, IPPROTO_TCP, TCP_NODELAY, 1);
	}

	while (c->wdata.len != 0) {
		int r = ffsock_send(c->sk, c->wdata.ptr, c->wdata.len, 0);
		if (r < 0) {
			if (!fferr_again(fferr_last())) {
				agg_syserr("sock send");
				conn_end(c);
				return;
			}
			conn_attach(c);
			c->whandler = conn_req_send;
			return;
		}

		ffstr_shift(&c->wdata, r);
		c->w->stats.total_sent += r;
	}

	c->start_time_usec = time_usec();
	conn_resp_recv(c);
}

static void conn_resp_recv(struct conn *c)
{
	for (;;) {
		int r = ffsock_recv(c->sk, c->buf + c->bufn, agg_conf->rbuf_size - c->bufn, 0);
		if (r < 0) {
			if (!fferr_again(fferr_last())) {
				agg_syserr("sock recv");
				break;
			}
			conn_attach(c);
			c->rhandler = conn_resp_recv;
			return;
		} else if (r == 0) {
			agg_err("server closed connection");
			break;
		}

		c->bufn += r;
		c->w->stats.total_recv += r;

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
		int r = ffsock_recv(c->sk, c->buf, n, 0);
		if (r < 0) {
			if (!fferr_again(fferr_last())) {
				agg_syserr("sock recv");
				break;
			}
			conn_attach(c);
			c->rhandler = conn_respdata_recv;
			return;
		} else if (r == 0) {
			agg_err("server closed connection");
			break;
		}

		c->cont_len -= r;
		c->w->stats.total_recv += r;
	}

	if (c->cont_len == 0) {
		if (c->resp_err)
			c->w->stats.resp_err++;
		else
			c->w->stats.resp_ok++;

		c->keepalive++;
		if (c->keepalive != agg_conf->keepalive_reqs) {
			agg_dbg("response finished");
			if (c->last) {
				agg_stopall();
				return;
			}
			conn_prep(c);

			int n = ffint_fetch_add(&agg_conf->total_reqs, -1);
			if (n <= 0) {
				return;
			} else if (n == 1) {
				c->last = 1;
			}

			conn_req_send(c);
			return;
		}
	}

	conn_end(c);
}

static void conn_end(struct conn *c)
{
	ffsock_close(c->sk);
	c->side = !c->side;
	agg_dbg("connection finished");
	if (c->last) {
		agg_stopall();
		return;
	}
	conn_start(c, c->w);
}
