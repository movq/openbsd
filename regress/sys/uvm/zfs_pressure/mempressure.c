/*
 * Allocate and touch anonymous memory gradually, hold it, then release it.
 * Multiple instances can be used to exceed the per-process data limit.
 */

#include <sys/types.h>
#include <sys/mman.h>

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void
handle_signal(int sig)
{
	stop = sig;
}

static void
sleep_msec(unsigned int msec)
{
	struct timespec delay;

	delay.tv_sec = msec / 1000;
	delay.tv_nsec = (msec % 1000) * 1000000L;
	while (!stop && nanosleep(&delay, &delay) == -1 && errno == EINTR)
		continue;
}

int
main(int argc, char **argv)
{
	const size_t mib = 1024 * 1024;
	size_t target, step, allocated = 0, nchunks, i;
	unsigned int delay_ms, hold_sec;
	long page_size;
	void **chunks;
	const char *errstr;

	if (argc != 5)
		errx(2, "usage: %s target-mib step-mib delay-ms hold-sec",
		    argv[0]);

	target = strtonum(argv[1], 1, 3072, &errstr);
	if (errstr != NULL)
		errx(2, "target-mib is %s: %s", errstr, argv[1]);
	step = strtonum(argv[2], 1, target, &errstr);
	if (errstr != NULL)
		errx(2, "step-mib is %s: %s", errstr, argv[2]);
	delay_ms = strtonum(argv[3], 0, 60000, &errstr);
	if (errstr != NULL)
		errx(2, "delay-ms is %s: %s", errstr, argv[3]);
	hold_sec = strtonum(argv[4], 1, 3600, &errstr);
	if (errstr != NULL)
		errx(2, "hold-sec is %s: %s", errstr, argv[4]);

	target *= mib;
	step *= mib;
	nchunks = (target + step - 1) / step;
	chunks = calloc(nchunks, sizeof (*chunks));
	if (chunks == NULL)
		err(1, "calloc");
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size == -1)
		err(1, "sysconf");

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	for (i = 0; i < nchunks && !stop; i++) {
		size_t chunk_size = target - allocated;
		volatile unsigned char *p;
		size_t off;

		if (chunk_size > step)
			chunk_size = step;
		chunks[i] = malloc(chunk_size);
		if (chunks[i] == NULL)
			err(1, "malloc at %zu MiB", allocated / mib);
		p = chunks[i];
		for (off = 0; off < chunk_size; off += page_size)
			p[off] = (unsigned char)i;
		p[chunk_size - 1] = (unsigned char)i;
		allocated += chunk_size;
		fprintf(stderr, "%lld allocated=%zu MiB\n",
		    (long long)time(NULL), allocated / mib);
		sleep_msec(delay_ms);
	}

	for (i = 0; i < hold_sec && !stop; i++)
		sleep(1);

	fprintf(stderr, "%lld releasing=%zu MiB signal=%d\n",
	    (long long)time(NULL), allocated / mib, stop);
	for (i = 0; i < nchunks; i++)
		free(chunks[i]);
	free(chunks);
	return (0);
}
