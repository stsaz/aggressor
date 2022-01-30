/** aggressor: startup
2022, Simon Zolin */

#include <aggressor.h>
#include <client.h>
#include <cmdline.h>
#include <util/ipaddr.h>
#include <util/http1.h>
#include <FFOS/signal.h>
#include <FFOS/ffos-extern.h>
#include <ffbase/atomic.h>
#include <sys/resource.h>
#include <assert.h>

int _ffcpu_features;
struct conf *agg_conf;

ffuint64 time_usec()
{
	fftime t = fftime_monotonic();
	return t.sec*1000000 + t.nsec/1000;
}

void stats()
{
	struct agg_stat s = {};
	struct worker *w;
	FFSLICE_WALK(&agg_conf->workers, w) {
		const struct agg_stat *ws = &w->stats;
		s.total_sent += ws->total_sent;
		s.total_recv += ws->total_recv;
		s.connections_ok += ws->connections_ok;
		s.connections_failed += ws->connections_failed;
		s.resp_ok += ws->resp_ok;
		s.resp_err += ws->resp_err;
		s.connect_latency_usec += ws->connect_latency_usec;
		s.resp_latency_usec += ws->resp_latency_usec;
	}
	s.connect_latency_usec /= agg_conf->workers.len;
	s.resp_latency_usec /= agg_conf->workers.len;

	ffuint64 t_ms = time_usec()/1000;
	t_ms -= agg_conf->start_time_usec/1000;
	ffstdout_fmt(
		"successful connections: %U\n"
		"failed connections:     %U\n"
		"successful responses:   %U\n"
		"failed responses:       %U\n"
		"time:                   %Umsec\n"
		"responses/sec:          %Urps\n"
		"total bytes sent:       %U\n"
		"total bytes received:   %U\n"
		"connection latency:     %Uusec\n"
		"response latency:       %Uusec\n"
		"\n"
		, s.connections_ok, s.connections_failed
		, s.resp_ok, s.resp_err
		, t_ms
		, (t_ms != 0) ? (s.resp_ok + s.resp_err) * 1000 / t_ms : 0ULL
		, s.total_sent, s.total_recv
		, s.connect_latency_usec
		, s.resp_latency_usec
		);
}

static int FFTHREAD_PROCCALL worker_func(void *param)
{
	struct worker *w = param;
	w->kq = ffkq_create();
	if (w->kq == FFKQ_NULL) {
		agg_syserr("kq create");
		return -1;
	}
	w->cpost = ffmem_new(struct conn);
	w->post = ffkq_post_attach(w->kq, w->cpost);

	uint n = agg_conf->connections_n / agg_conf->workers.len;
	w->connections = ffmem_alloc(n * (sizeof(struct conn) + agg_conf->rbuf_size));
	char *ptr = (void*)w->connections;
	for (uint i = 0;  i != n;  i++) {
		struct conn *c = (void*)ptr;
		ptr += sizeof(struct conn) + agg_conf->rbuf_size;
		conn_start(c, w);
	}

	w->kevents = ffmem_alloc(agg_conf->events_num * sizeof(ffkq_event));

	ffkq_time t;
	ffkq_time_set(&t, -1);
	while (!FFINT_READONCE(w->worker_stop)) {
		int r = ffkq_wait(w->kq, w->kevents, agg_conf->events_num, t);

		for (int i = 0;  i < r;  i++) {
			ffkq_event *ev = &w->kevents[i];
			void *d = ffkq_event_data(ev);
			struct conn *c = (void*)((ffsize)d & ~1);

			if (((ffsize)d & 1) != c->side)
				continue;

			c->kev_flags = ev->events;
			int flags = ffkq_event_flags(ev);
			if ((flags & FFKQ_READ) && c->rhandler != NULL)
				c->rhandler(c);
			if ((flags & FFKQ_WRITE) && c->whandler != NULL)
				c->whandler(c);
		}

		if (r < 0 && fferr_last() != EINTR) {
			agg_syserr("kq wait");
			return -1;
		}
	}

	ffmem_free(w->kevents);
	ffkq_close(w->kq);
	return 0;
}

static void run()
{
	agg_conf->start_time_usec = time_usec();

	ffvec_allocT(&agg_conf->workers, agg_conf->threads, struct worker);
	ffmem_zero(agg_conf->workers.ptr, agg_conf->threads * sizeof(struct worker));
	agg_conf->workers.len = agg_conf->threads - 1;
	struct worker *w;
	FFSLICE_WALK(&agg_conf->workers, w) {
		w->t = ffthread_create(worker_func, w, 0);
		assert(w->t != FFTHREAD_NULL);
	}

	w = ffvec_pushT(&agg_conf->workers, struct worker);
	w->t = FFTHREAD_NULL;
	worker_func(w);

	FFSLICE_WALK(&agg_conf->workers, w) {
		if (w->t != FFTHREAD_NULL)
			ffthread_join(w->t, -1, NULL);
	}
}

void agg_stopall()
{
	struct worker *w;
	FFSLICE_WALK(&agg_conf->workers, w) {
		FFINT_WRITEONCE(w->worker_stop, 1);
		ffkq_post(w->post, w->cpost);
	}
}

static void sig_handler(struct ffsig_info *i)
{
	agg_stopall();
}

int main(int argc, char **argv)
{
	agg_conf = ffmem_new(struct conf);
	if (0 != cmd_process(agg_conf, argc, (const char **)argv))
		goto end;

	if (agg_conf->fd_limit != 0) {
		struct rlimit rl;
		rl.rlim_cur = agg_conf->fd_limit;
		rl.rlim_max = agg_conf->fd_limit;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	ffuint sigs = SIGINT;
	ffsig_subscribe(sig_handler, &sigs, 1);

	run();
	stats();

end:
	cmd_destroy(agg_conf);
	ffmem_free(agg_conf);
	return 0;
}
