# aggressor

It's a tool to perform stress testing on an HTTP/1.1 server.
Easily maxes out nginx (static files or 404 errors) on the same hardware (`~140krps` on my PC with 2 threads vs 6 for nginx, with 64 keep-alive requests).

Features and limitations:

* Runs on Linux and FreeBSD (uses epoll and kqueue)
* Multi-threaded, uses all CPUs by default
* Keep-alive
* Doesn't support chunked response
* One target server
* Multiple target paths
* Custom HTTP method and headers

Build on Linux:

	git clone https://github.com/stsaz/ffbase
	git clone https://github.com/stsaz/ffos
	git clone https://github.com/stsaz/aggressor
	cd aggressor
	make -j4

Run until manually stopped:

	./aggressor 127.0.0.1:8080/index.html 127.0.0.1:8080/s.css

Use 6 threads, 500 parallel connections, stop after 100k requests:

	./aggressor 127.0.0.1:8080/index.html 127.0.0.1:8080/s.css -t 6 -c 500 -n 100000

Example output:

	successful connections: 13971
	failed connections:     0
	successful responses:   890933
	failed responses:       0
	time:                   6308msec
	responses/sec:          141238rps
	total bytes sent:       44551650
	total bytes received:   757293050
	connection latency:     23usec
	response latency:       648usec
