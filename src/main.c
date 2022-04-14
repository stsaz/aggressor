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

static void stats()
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

	ffuint64 t_ms = (time_usec() - agg_conf->start_time_usec) / 1000;
	ffstdout_fmt(
		"time:                   %20Umsec\n"
		"successful connections: %20U\n"
		"failed connections:     %20U\n"
		"successful responses:   %20U\n"
		"failed responses:       %20U\n"
		"responses/sec:          %20Urps\n"
		"total sent:             %20UB\n"
		"total received:         %20UB\n"
		"send/sec:               %20Ubps\n"
		"receive/sec:            %20Ubps\n"
		"connection latency:     %20Uusec\n"
		"response latency:       %20Uusec\n"
		"\n"
		, t_ms
		, s.connections_ok, s.connections_failed
		, s.resp_ok, s.resp_err
		, (t_ms != 0) ? (s.resp_ok + s.resp_err) * 1000 / t_ms : 0ULL
		, s.total_sent, s.total_recv
		, (t_ms != 0) ? s.total_sent*8 / t_ms : 0ULL
		, (t_ms != 0) ? s.total_recv*8 / t_ms : 0ULL
		, s.connect_latency_usec
		, s.resp_latency_usec
		);
}

#ifdef FF_LINUX
typedef cpu_set_t _cpuset;
#else
typedef cpuset_t _cpuset;
#endif

static int FFTHREAD_PROCCALL worker_func(void *param)
{
	struct worker *w = param;

	if (w->icpu >= 0) {
		_cpuset cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(w->icpu, &cpuset);
		if (0 == pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset))
			agg_dbg("CPU affinity: %u", w->icpu);
	}

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

			ffkq_task_event_assign(&c->kqtask, ev);
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
	agg_conf->workers.len = agg_conf->threads;
	struct worker *w;
	uint mask = agg_conf->cpumask;
	FFSLICE_WALK(&agg_conf->workers, w) {

		w->icpu = -1;
		uint n = ffbit_rfind32(mask);
		if (n != 0) {
			n--;
			ffbit_reset32(&mask, n);
			w->icpu = n;
		}

		if (w == agg_conf->workers.ptr)
			continue;

		w->t = ffthread_create(worker_func, w, 0);
		assert(w->t != FFTHREAD_NULL);
	}

	w = (void*)agg_conf->workers.ptr;
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
	static char appname[] = "aggressor " AGG_VER "\n";
	ffstdout_write(appname, FFS_LEN(appname));

	agg_conf = ffmem_new(struct conf);
	if (0 != cmd_process(agg_conf, argc, (const char **)argv))
		goto end;

	if (agg_conf->fd_limit != 0) {
		struct rlimit rl;
		rl.rlim_cur = agg_conf->fd_limit;
		rl.rlim_max = agg_conf->fd_limit;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	ffsock_init(FFSOCK_INIT_SIGPIPE | FFSOCK_INIT_WSA | FFSOCK_INIT_WSAFUNCS);

	ffuint sigs = SIGINT;
	ffsig_subscribe(sig_handler, &sigs, 1);

	run();
	stats();

end:
	cmd_destroy(agg_conf);
	ffmem_free(agg_conf);
	return 0;
}
