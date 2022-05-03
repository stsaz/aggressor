/** aggressor: shared data
2022, Simon Zolin */

#include <FFOS/socket.h>
#include <FFOS/queue.h>
#include <FFOS/thread.h>
#include <FFOS/std.h>
#include <FFOS/perf.h>
#include <FFOS/error.h>
#include <ffbase/string.h>
#include <ffbase/vector.h>

#define AGG_VER  "0.3"

typedef unsigned int uint;

struct conn;
struct conf {
	ffsockaddr addr;
	uint threads;
	uint connections_n;
	uint keepalive_reqs;
	uint total_reqs;
	uint fd_limit;
	uint events_num;
	uint rbuf_size;
	uint debug;
	uint cpumask; // 0:disable
	ffstr method;
	ffvec paths; // ffstr[]
	ffvec headers;
	ffvec reqs; // ffstr[];  The prepared request data ready to send

	ffvec workers;
	ffuint64 start_time_usec;
	uint nreqs;
};
extern struct conf *agg_conf;

struct agg_stat {
	ffuint64 total_sent, total_recv;
	ffuint64 connections_ok, connections_failed, resp_ok, resp_err;
	ffuint64 connect_latency_usec, resp_latency_usec;
};

struct worker {
	struct conn *connections;
	ffkq kq;
	ffthread t;
	ffkq_event *kevents;
	int icpu; // -1:disable affinity
	uint worker_stop;
	uint next_req;
	ffkq_postevent post;
	struct conn *cpost;

	struct agg_stat stats;
};

typedef void (*kev_handler)(struct conn *c);
struct conn {
	kev_handler rhandler, whandler;
	uint side;
	ffkq_task kqtask, kqtask2;

	ffsock sk;
	struct worker *w;
	uint keepalive;
	unsigned kq_attach_ok :1;
	// next data is cleared on each new request

	ffstr wdata;
	ffuint64 start_time_usec;
	ffuint64 cont_len;
	unsigned resp_line_ok :1;
	unsigned resp_err :1;
	uint bufn;
	char buf[0];
};

#define agg_dbg(fmt, ...) \
do { \
	if (agg_conf->debug) \
		ffstderr_fmt("dbg: " fmt "\n", ##__VA_ARGS__); \
} while(0)
#define agg_err(fmt, ...) \
	ffstderr_fmt("error: " fmt "\n", ##__VA_ARGS__)
#define agg_syserr(fmt, ...) \
	ffstderr_fmt("error: " fmt ": %s\n", ##__VA_ARGS__, fferr_strptr(fferr_last()))


/**
closed: whether connection is closed
Return 1: all done */
int agg_conn_fin(struct conn *c, int closed);

ffuint64 time_usec();


void conn_start(struct conn *c, struct worker *w);
void conn_close(struct conn *c);
