/** aggressor: command line arguments
2022, Simon Zolin */

#include <util/cmdarg-scheme.h>
#include <util/ipaddr.h>
#include <util/http1.h>
#include <FFOS/sysconf.h>

#define CONF_RDONE  100

static int cmd_url(ffcmdarg_scheme *as, struct conf *c, ffstr *s)
{
	ffstr *p = ffvec_pushT(&c->paths, ffstr);
	ffmem_zero_obj(p);
	ffstr_dupstr(p, s);
	return 0;
}

static int cmd_header(ffcmdarg_scheme *as, struct conf *c, ffstr *s)
{
	ffvec_addfmt(&c->headers, "%S\r\n", s);
	return 0;
}

static int cmd_cpuaffinity(ffcmdarg_scheme *as, struct conf *c, ffstr *val)
{
	if (!ffstr_toint(val, &c->cpumask, FFS_INT32 | FFS_INTHEX))
		return FFCMDARG_ERROR;
	return 0;
}

static int cmd_usage()
{
	static const char usage[] =
"aggressor [OPTIONS] URL...\n"
"URL: request URL (e.g. \"127.0.0.1:8080/file\")\n"
" Host names here are NOT supported\n"
"Options:\n"
" -n, --number N       Total number of requests (def: unlimited)\n"
" -c, --concurrency N  Concurrent connectons (def: 100)\n"
" -t, --threads N      Worker threads (def: CPU#)\n"
" -a, --affinity N     CPU affinity bitmask, hex value (e.g. 15 for CPUs 0,2,4)\n"
" -k, --keepalive N    Max. keep-alive requests per connection (def: 64)\n"
" -m, --method STR     HTTP request method (def: GET)\n"
" -H, --header STR     Add HTTP request header\n"
" -D, --debug          Debug logging\n"
" -h, --help           Show help\n"
;
	ffstdout_write(usage, FFS_LEN(usage));
	return CONF_RDONE;
}

static const ffcmdarg_arg cmd_args[] = {
	{ 0, "",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY | FFCMDARG_FMULTI, (ffsize)cmd_url },
	{ 'n', "number",	FFCMDARG_TINT32, FF_OFF(struct conf, total_reqs) },
	{ 'c', "concurrency",	FFCMDARG_TINT32, FF_OFF(struct conf, connections_n) },
	{ 't', "threads",	FFCMDARG_TINT32, FF_OFF(struct conf, threads) },
	{ 'a', "affinity",	FFCMDARG_TSTR, (ffsize)cmd_cpuaffinity },
	{ 'k', "keepalive",	FFCMDARG_TINT32, FF_OFF(struct conf, keepalive_reqs) },
	{ 'm', "method",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, FF_OFF(struct conf, method) },
	{ 'H', "header",	FFCMDARG_TSTR | FFCMDARG_FMULTI | FFCMDARG_FNOTEMPTY, (ffsize)cmd_header },
	{ 'D', "debug",	FFCMDARG_TSWITCH, FF_OFF(struct conf, debug) },
	{ 'h', "help",	FFCMDARG_TSWITCH, (ffsize)cmd_usage },
	{}
};

static void cmd_init(struct conf *c)
{
	c->total_reqs = 0x7fffffff;
	c->keepalive_reqs = 64;
	c->connections_n = 100;
	c->events_num = 512;
	c->rbuf_size = 4096;
	ffstr_dupz(&c->method, "GET");
}

static void cmd_destroy(struct conf *c)
{
	ffvec_free(&c->headers);

	ffstr *it;
	FFSLICE_WALK(&c->paths, it) {
		ffstr_free(it);
	}
	ffvec_free(&c->paths);

	FFSLICE_WALK(&c->reqs, it) {
		ffstr_free(it);
	}
	ffvec_free(&c->reqs);

	ffvec_free(&c->workers);
	ffstr_free(&c->method);
}

static int cmd_finalize(struct conf *c)
{
	if (c->paths.len == 0) {
		agg_err("URL is empty");
		return -1;
	}

	ffstr *it;
	FFSLICE_WALK(&c->paths, it) {
		struct httpurl_parts u = {};
		httpurl_split(&u, *it);

		uint port = 80;
		if (u.port.len != 0) {
			ffstr_shift(&u.port, 1);
			if (!ffstr_to_uint32(&u.port, &port)
				|| port == 0 || port > 0xffff) {
				agg_err("bad port");
				return -1;
			}
		}

		char ip[16];
		if (0 == ffip4_parse((void*)ip, u.host.ptr, u.host.len)) {
			ffsockaddr_set_ipv4(&agg_conf->addr, ip, port);
		} else if (0 == ffip6_parse((void*)ip, u.host.ptr, u.host.len)) {
			ffsockaddr_set_ipv6(&agg_conf->addr, ip, port);
		} else {
			agg_err("bad IP address");
			return -1;
		}

		if (u.path.len == 0)
			ffstr_setz(&u.path, "/");

		ffstr *ps = ffvec_pushT(&c->reqs, ffstr);
		ffmem_zero_obj(ps);
		ffsize cap = 4096;
		ffstr_alloc(ps, cap);
		ps->len = http_req_write(ps->ptr, cap, agg_conf->method, u.path, 0);
		ffstr_growfmt(ps, &cap, "Host: %S:%u\r\n", &u.host, port);
		ffstr_growadd2(ps, &cap, &agg_conf->headers);
		ffstr_growaddz(ps, &cap, "\r\n");
	}

	if (c->threads == 0) {
		ffsysconf sc;
		ffsysconf_init(&sc);
		c->threads = ffsysconf_get(&sc, FFSYSCONF_NPROCESSORS_ONLN);
		if (c->cpumask == 0)
			c->cpumask = (uint)-1;
	}

	if (c->connections_n > 1024)
		c->fd_limit = c->connections_n * 2;
	return 0;
}

int cmd_process(struct conf *c, int argc, const char **argv)
{
	cmd_init(c);

	ffstr errmsg = {};
	int r = ffcmdarg_parse_object(cmd_args, c, argv, argc, 0, &errmsg);
	if (r == -CONF_RDONE)
		return -1;
	else if (r != 0) {
		agg_err("command line: %S", &errmsg);
		return -1;
	}
	if (0 != cmd_finalize(c))
		return -1;
	return 0;
}
