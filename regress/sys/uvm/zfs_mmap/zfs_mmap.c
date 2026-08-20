/* Public domain. */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char testdir[PATH_MAX];
static char selfpath[PATH_MAX];
static size_t pagesize;
static unsigned int iterations = 500;

static void	check_bytes(const volatile unsigned char *,
		    const unsigned char *, size_t, const char *);
static void	check_value(const volatile unsigned char *, unsigned char,
		    size_t, const char *);
static void	fill_pattern(unsigned char *, size_t, uint64_t);
static int	make_file(const char *, off_t);
static void	make_path(char *, size_t, const char *);
static void	read_at(int, void *, size_t, off_t);
static void	write_at(int, const void *, size_t, off_t);
static void	wait_ok(pid_t, const char *);
static void	expect_sigbus(volatile unsigned char *, const char *);

static void	test_shared_read(void);
static void	test_shared_write(void);
static void	test_shared_fork(void);
static void	test_private_cow(void);
static void	test_protection(void);
static void	test_write_coherence(void);
static void	test_write_merge(void);
static void	test_truncate(void);
static void	test_dirty_truncate(void);
static void	test_unlink(void);
static void	test_self_io(void);
static void	test_exec(void);
static void	test_write_race(void);
static void	test_shared_write_race(void);
static void	test_truncate_race(void);

struct testcase {
	const char *name;
	void (*func)(void);
};

static const struct testcase tests[] = {
	{ "shared_read", test_shared_read },
	{ "shared_write", test_shared_write },
	{ "shared_fork", test_shared_fork },
	{ "private_cow", test_private_cow },
	{ "protection", test_protection },
	{ "write_coherence", test_write_coherence },
	{ "write_merge", test_write_merge },
	{ "truncate", test_truncate },
	{ "dirty_truncate", test_dirty_truncate },
	{ "unlink", test_unlink },
	{ "self_io", test_self_io },
	{ "exec", test_exec },
	{ "write_race", test_write_race },
	{ "shared_write_race", test_shared_write_race },
	{ "truncate_race", test_truncate_race },
};

static void
make_path(char *path, size_t len, const char *name)
{
	int n;

	n = snprintf(path, len, "%s/zfs_mmap.%ld.%s", testdir,
	    (long)getpid(), name);
	if (n < 0 || (size_t)n >= len)
		errx(1, "test pathname is too long");
}

static int
make_file(const char *name, off_t size)
{
	char path[PATH_MAX];
	int fd;

	make_path(path, sizeof(path), name);
	(void)unlink(path);
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd == -1)
		err(1, "open: %s", path);
	if (ftruncate(fd, size) == -1)
		err(1, "ftruncate: %s", path);
	return (fd);
}

static void
write_at(int fd, const void *vbuf, size_t len, off_t off)
{
	const unsigned char *buf = vbuf;
	ssize_t n;

	while (len != 0) {
		n = pwrite(fd, buf, len, off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "pwrite");
		}
		if (n == 0)
			errx(1, "pwrite made no progress");
		buf += n;
		len -= n;
		off += n;
	}
}

static void
read_at(int fd, void *vbuf, size_t len, off_t off)
{
	unsigned char *buf = vbuf;
	ssize_t n;

	while (len != 0) {
		n = pread(fd, buf, len, off);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "pread");
		}
		if (n == 0)
			errx(1, "unexpected end of file at %lld",
			    (long long)off);
		buf += n;
		len -= n;
		off += n;
	}
}

static void
fill_pattern(unsigned char *buf, size_t len, uint64_t base)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)(((base + i) * 131 + 17) & 0xff);
}

static void
check_bytes(const volatile unsigned char *got, const unsigned char *want,
    size_t len, const char *what)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (got[i] != want[i])
			errx(1, "%s: byte %zu is %#x, expected %#x", what, i,
			    got[i], want[i]);
	}
}

static void
check_value(const volatile unsigned char *got, unsigned char want,
    size_t len, const char *what)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (got[i] != want)
			errx(1, "%s: byte %zu is %#x, expected %#x", what, i,
			    got[i], want);
	}
}

static void
wait_ok(pid_t pid, const char *what)
{
	int status;

	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid: %s", what);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFSIGNALED(status))
			errx(1, "%s: child died from signal %d", what,
			    WTERMSIG(status));
		errx(1, "%s: child exit status %d", what,
		    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	}
}

static void
fault_handler(int signo)
{

	_exit(signo);
}

static void
expect_sigbus(volatile unsigned char *addr, const char *what)
{
	volatile unsigned char value;
	pid_t pid;
	int status;

	pid = fork();
	if (pid == -1)
		err(1, "fork: %s", what);
	if (pid == 0) {
		if (signal(SIGBUS, fault_handler) == SIG_ERR ||
		    signal(SIGSEGV, fault_handler) == SIG_ERR)
			_exit(125);
		value = *addr;
		(void)value;
		_exit(0);
	}
	if (waitpid(pid, &status, 0) == -1)
		err(1, "waitpid: %s", what);
	if (WIFEXITED(status) && WEXITSTATUS(status) == SIGBUS)
		return;
	if (WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS)
		return;
	if (WIFEXITED(status))
		errx(1, "%s: expected SIGBUS, child exited %d", what,
		    WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		errx(1, "%s: expected SIGBUS, child got signal %d", what,
		    WTERMSIG(status));
	errx(1, "%s: child did not exit normally", what);
}

static void
test_shared_read(void)
{
	unsigned char *buf, *want;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len, sparse_len, i;
	int fd;

	len = pagesize * 3 + 137;
	buf = malloc(len);
	if (buf == NULL)
		err(1, "malloc");
	fill_pattern(buf, len, 0);
	fd = make_file("shared_read", len);
	write_at(fd, buf, len, 0);

	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap shared_read");
	check_bytes(map, buf, len, "shared read");
	if (munmap((void *)map, len) == -1)
		err(1, "munmap shared_read");

	map = mmap(NULL, pagesize + 137, PROT_READ, MAP_SHARED, fd,
	    pagesize);
	if (map == MAP_FAILED)
		err(1, "mmap non-zero offset");
	check_bytes(map, buf + pagesize, pagesize + 137,
	    "shared read at non-zero offset");
	if (munmap((void *)map, pagesize + 137) == -1)
		err(1, "munmap non-zero offset");

	sparse_len = pagesize * 5;
	if (ftruncate(fd, sparse_len) == -1)
		err(1, "ftruncate sparse file");
	write_at(fd, "Z", 1, sparse_len - 1);
	want = calloc(1, sparse_len);
	if (want == NULL)
		err(1, "calloc");
	memcpy(want, buf, len);
	want[sparse_len - 1] = 'Z';
	map = mmap(NULL, sparse_len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap sparse file");
	check_bytes(map, want, sparse_len, "sparse shared read");
	for (i = len; i < sparse_len - 1; i++) {
		if (map[i] != 0)
			errx(1, "sparse hole: byte %zu is %#x", i, map[i]);
	}
	if (munmap((void *)map, sparse_len) == -1)
		err(1, "munmap sparse file");
	if (close(fd) == -1)
		err(1, "close shared_read");
	make_path(path, sizeof(path), "shared_read");
	if (unlink(path) == -1)
		err(1, "unlink shared_read");
	free(want);
	free(buf);
}

static void
test_private_cow(void)
{
	unsigned char *buf, disk[2];
	volatile unsigned char *map, *shared;
	char path[PATH_MAX];
	size_t len;
	pid_t pid;
	int fd;

	len = pagesize * 2;
	buf = malloc(len);
	if (buf == NULL)
		err(1, "malloc");
	fill_pattern(buf, len, 19);
	fd = make_file("private_cow", len);
	write_at(fd, buf, len, 0);

	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap private_cow");
	check_bytes(map, buf, len, "private initial contents");
	map[0] = 0xa5;
	map[pagesize + 7] = 0x5a;
	read_at(fd, disk, 1, 0);
	read_at(fd, disk + 1, 1, pagesize + 7);
	if (disk[0] != buf[0] || disk[1] != buf[pagesize + 7])
		errx(1, "private writes changed file contents");

	shared = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (shared == MAP_FAILED)
		err(1, "mmap shared alias");
	check_bytes(shared, buf, len, "file after private writes");

	pid = fork();
	if (pid == -1)
		err(1, "fork private_cow");
	if (pid == 0) {
		map[23] = 0xcc;
		_exit(map[23] == 0xcc && map[0] == 0xa5 ? 0 : 1);
	}
	wait_ok(pid, "private fork COW");
	if (map[23] != buf[23])
		errx(1, "child private write changed parent mapping");

	if (munmap((void *)shared, len) == -1 ||
	    munmap((void *)map, len) == -1)
		err(1, "munmap private_cow");
	if (close(fd) == -1)
		err(1, "close private_cow");
	make_path(path, sizeof(path), "private_cow");
	if (unlink(path) == -1)
		err(1, "unlink private_cow");
	free(buf);
}

static void
test_protection(void)
{
	volatile unsigned char *map;
	unsigned char before, after;
	char path[PATH_MAX];
	int fd;

	fd = make_file("protection", pagesize);
	write_at(fd, "Q", 1, 0);

	map = mmap(NULL, pagesize, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "read-only MAP_SHARED");
	if (map[0] != 'Q')
		errx(1, "read-only MAP_SHARED returned bad data");
	if (mprotect((void *)map, pagesize, PROT_READ | PROT_WRITE) == -1)
		err(1, "mprotect shared mapping read-write");
	map[0] = 'R';
	read_at(fd, &after, 1, 0);
	if (after != 'R')
		errx(1, "mprotect write not visible through read");
	if (munmap((void *)map, pagesize) == -1)
		err(1, "munmap protection");

	map = mmap(NULL, pagesize, PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "write-only MAP_SHARED");
	map[1] = 'W';
	if (munmap((void *)map, pagesize) == -1)
		err(1, "munmap write-only protection");

	map = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "read-write MAP_SHARED");
	if (map[0] != 'R' || map[1] != 'W')
		errx(1, "shared protection changes lost");
	if (munmap((void *)map, pagesize) == -1)
		err(1, "munmap read-write protection");

	read_at(fd, &before, 1, 0);
	map = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		err(1, "read-write MAP_PRIVATE");
	map[0] ^= 0xff;
	read_at(fd, &after, 1, 0);
	if (before != after)
		errx(1, "MAP_PRIVATE write changed file");
	if (munmap((void *)map, pagesize) == -1)
		err(1, "munmap private protection");

	if (close(fd) == -1)
		err(1, "close protection");
	make_path(path, sizeof(path), "protection");
	if (unlink(path) == -1)
		err(1, "unlink protection");
}

static void
test_shared_write(void)
{
	unsigned char *want, *got;
	volatile unsigned char *map, *alias;
	char path[PATH_MAX];
	size_t len, i;
	int fd;

	len = pagesize * 3;
	want = malloc(len);
	got = malloc(len);
	if (want == NULL || got == NULL)
		err(1, "malloc");
	fill_pattern(want, len, 211);
	fd = make_file("shared_write", len);
	write_at(fd, want, len, 0);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap shared_write");
	alias = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (alias == MAP_FAILED)
		err(1, "mmap shared_write alias");

	for (i = pagesize - 31; i < pagesize + 47; i++)
		map[i] = want[i] = 0xc3;
	map[2 * pagesize + 19] = want[2 * pagesize + 19] = 0x4d;
	check_bytes(alias, want, len, "shared alias after mapped stores");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "pread after mapped stores");

	if (msync((void *)map, len, MS_SYNC) == -1)
		err(1, "msync shared_write");
	map[17] = want[17] = 0x91;
	if (fsync(fd) == -1)
		err(1, "fsync shared_write");
	if (munmap((void *)alias, len) == -1 ||
	    munmap((void *)map, len) == -1)
		err(1, "munmap shared_write");
	if (close(fd) == -1)
		err(1, "close shared_write");

	make_path(path, sizeof(path), "shared_write");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "reopen shared_write");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "persisted shared stores");
	if (close(fd) == -1 || unlink(path) == -1)
		err(1, "cleanup shared_write");
	free(got);
	free(want);
}

static void
test_shared_fork(void)
{
	unsigned char *want, *got;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len, i;
	pid_t pid;
	int fd;

	len = pagesize * 2;
	want = calloc(1, len);
	got = malloc(len);
	if (want == NULL || got == NULL)
		err(1, "malloc");
	fd = make_file("shared_fork", len);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap shared_fork");
	pid = fork();
	if (pid == -1)
		err(1, "fork shared_fork");
	if (pid == 0) {
		for (i = pagesize - 17; i < pagesize + 29; i++)
			map[i] = 0x7b;
		_exit(0);
	}
	wait_ok(pid, "shared mapping fork writer");
	for (i = pagesize - 17; i < pagesize + 29; i++)
		want[i] = 0x7b;
	check_bytes(map, want, len, "parent view after child stores");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "pread after child stores");
	if (msync((void *)map, len, MS_SYNC) == -1)
		err(1, "msync shared_fork");
	if (munmap((void *)map, len) == -1 || close(fd) == -1)
		err(1, "close shared_fork");
	make_path(path, sizeof(path), "shared_fork");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "reopen shared_fork");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "persisted child stores");
	if (close(fd) == -1 || unlink(path) == -1)
		err(1, "cleanup shared_fork");
	free(got);
	free(want);
}

static void
test_write_coherence(void)
{
	unsigned char *want, *update;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len, partial;
	int fd;

	len = pagesize * 4;
	want = malloc(len);
	update = malloc(pagesize);
	if (want == NULL || update == NULL)
		err(1, "malloc");
	fill_pattern(want, len, 47);
	fd = make_file("write_coherence", len);
	write_at(fd, want, len, 0);
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap write_coherence");

	/* Fault every page before changing the file through write(2). */
	check_bytes(map, want, len, "coherence initial contents");
	memset(update, 0xa6, pagesize);
	write_at(fd, update, pagesize, pagesize);
	memcpy(want + pagesize, update, pagesize);
	check_bytes(map, want, len, "coherence after full-page pwrite");

	partial = pagesize - 62;
	memset(update, 0x6a, partial);
	write_at(fd, update, partial, pagesize * 2 + 31);
	memcpy(want + pagesize * 2 + 31, update, partial);
	/* No msync(2) or madvise(2): they could hide failed invalidation. */
	check_bytes(map, want, len, "coherence after partial pwrite");

	if (munmap((void *)map, len) == -1)
		err(1, "munmap write_coherence");
	if (close(fd) == -1)
		err(1, "close write_coherence");
	make_path(path, sizeof(path), "write_coherence");
	if (unlink(path) == -1)
		err(1, "unlink write_coherence");
	free(update);
	free(want);
}

static void
test_write_merge(void)
{
	unsigned char *want, *got, update[97];
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len;
	int fd;

	len = pagesize * 2;
	want = malloc(len);
	got = malloc(len);
	if (want == NULL || got == NULL)
		err(1, "malloc");
	fill_pattern(want, len, 313);
	fd = make_file("write_merge", len);
	write_at(fd, want, len, 0);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap write_merge");
	check_bytes(map, want, len, "write_merge initial contents");

	/* Dirty bytes on both sides of an ordinary write to the same page. */
	map[7] = want[7] = 0xd1;
	map[pagesize - 9] = want[pagesize - 9] = 0xd2;
	memset(update, 0x3e, sizeof(update));
	write_at(fd, update, sizeof(update), 101);
	memcpy(want + 101, update, sizeof(update));
	check_bytes(map, want, pagesize, "mapping after partial pwrite");
	read_at(fd, got, pagesize, 0);
	check_bytes(got, want, pagesize, "pread after dirty-page merge");

	/* Repeat across a page boundary to exercise both resident pages. */
	map[pagesize + 211] = want[pagesize + 211] = 0xe7;
	memset(update, 0x62, sizeof(update));
	write_at(fd, update, sizeof(update), pagesize - 41);
	memcpy(want + pagesize - 41, update, sizeof(update));
	check_bytes(map, want, len, "mapping after cross-page pwrite");
	if (msync((void *)map, len, MS_SYNC) == -1)
		err(1, "msync write_merge");
	if (munmap((void *)map, len) == -1 || close(fd) == -1)
		err(1, "close write_merge");
	make_path(path, sizeof(path), "write_merge");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "reopen write_merge");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "persisted dirty-page merge");
	if (close(fd) == -1 || unlink(path) == -1)
		err(1, "cleanup write_merge");
	free(got);
	free(want);
}

static void
test_truncate(void)
{
	unsigned char *want;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len, shortlen;
	int fd;

	len = pagesize * 3;
	shortlen = pagesize + 37;
	want = malloc(len);
	if (want == NULL)
		err(1, "malloc");
	fill_pattern(want, len, 71);
	fd = make_file("truncate", len);
	write_at(fd, want, len, 0);
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap truncate");
	check_bytes(map, want, len, "truncate initial contents");

	if (ftruncate(fd, shortlen) == -1)
		err(1, "ftruncate partial page");
	check_bytes(map, want, shortlen, "contents preserved by truncate");
	check_value(map + shortlen, 0, pagesize * 2 - shortlen,
	    "truncated partial page tail");
	expect_sigbus(map + pagesize * 2, "page beyond truncated EOF");

	if (ftruncate(fd, 0) == -1)
		err(1, "ftruncate zero");
	expect_sigbus(map, "first page after truncate to zero");
	if (ftruncate(fd, len) == -1)
		err(1, "regrow truncated file");
	check_value(map, 0, len, "regrown file through existing mapping");

	if (munmap((void *)map, len) == -1)
		err(1, "munmap truncate");
	if (close(fd) == -1)
		err(1, "close truncate");
	make_path(path, sizeof(path), "truncate");
	if (unlink(path) == -1)
		err(1, "unlink truncate");
	free(want);
}

static void
test_dirty_truncate(void)
{
	unsigned char *want, *got;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len, shortlen;
	int fd;

	len = pagesize * 3;
	shortlen = pagesize + 83;
	want = calloc(1, len);
	got = malloc(len);
	if (want == NULL || got == NULL)
		err(1, "malloc");
	fd = make_file("dirty_truncate", len);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap dirty_truncate");

	map[37] = want[37] = 0xa1;
	map[pagesize + 17] = want[pagesize + 17] = 0xa2;
	map[2 * pagesize + 17] = 0xff;
	/* ftruncate must clean the retained dirty bytes before shrinking. */
	if (ftruncate(fd, shortlen) == -1)
		err(1, "dirty ftruncate shrink");
	check_bytes(map, want, shortlen, "retained dirty mapping after shrink");
	if (ftruncate(fd, len) == -1)
		err(1, "dirty ftruncate regrow");
	check_value(map + shortlen, 0, len - shortlen,
	    "regrown tail after dirty truncate");
	read_at(fd, got, len, 0);
	check_bytes(got, want, len, "pread after dirty truncate and regrow");
	if (msync((void *)map, len, MS_SYNC) == -1)
		err(1, "msync dirty_truncate");
	if (munmap((void *)map, len) == -1 || close(fd) == -1)
		err(1, "close dirty_truncate");
	make_path(path, sizeof(path), "dirty_truncate");
	if (unlink(path) == -1)
		err(1, "unlink dirty_truncate");
	free(got);
	free(want);
}

static void
test_unlink(void)
{
	unsigned char *want;
	volatile unsigned char *map;
	char path[PATH_MAX];
	size_t len;
	int fd;

	/* Keep the post-unlink fault well beyond UVM's initial read cluster. */
	len = pagesize * 128;
	want = malloc(len);
	if (want == NULL)
		err(1, "malloc");
	fill_pattern(want, len, 103);
	fd = make_file("unlink", len);
	write_at(fd, want, len, 0);
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap unlink");
	/* Leave the distant final page unfaulted until there is no pathname/fd. */
	check_bytes(map, want, pagesize, "unlink first page");
	make_path(path, sizeof(path), "unlink");
	if (unlink(path) == -1)
		err(1, "unlink mapped file");
	if (close(fd) == -1)
		err(1, "close unlinked file");
	check_bytes(map + len - pagesize, want + len - pagesize, pagesize,
	    "fault after unlink and close");
	if (munmap((void *)map, len) == -1)
		err(1, "munmap unlink");
	free(want);
}

static void
test_self_io(void)
{
	unsigned char *want;
	volatile unsigned char *map;
	char path[PATH_MAX];
	ssize_t n;
	int fd, saved_errno;

	want = malloc(pagesize);
	if (want == NULL)
		err(1, "malloc");
	fill_pattern(want, pagesize, 127);
	fd = make_file("self_io", pagesize);
	write_at(fd, want, pagesize, 0);
	map = mmap(NULL, pagesize, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap self_io");

	/* Do not prefault map: copyin must fault while writing the same vnode. */
	errno = 0;
	n = pwrite(fd, (const void *)map, pagesize, 0);
	saved_errno = errno;
	if (n != -1 || saved_errno != EFAULT)
		errx(1, "same-vnode pwrite returned %zd errno %d, expected -1/%d",
		    n, saved_errno, EFAULT);
	check_bytes(map, want, pagesize, "self_io contents after failed write");

	if (munmap((void *)map, pagesize) == -1)
		err(1, "munmap self_io");
	if (close(fd) == -1)
		err(1, "close self_io");
	make_path(path, sizeof(path), "self_io");
	if (unlink(path) == -1)
		err(1, "unlink self_io");
	free(want);
}

static void
copy_self(const char *path)
{
	unsigned char buf[16384];
	ssize_t n, written;
	int from, to;

	from = open(selfpath, O_RDONLY);
	if (from == -1)
		err(1, "open self: %s", selfpath);
	to = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
	if (to == -1)
		err(1, "create executable: %s", path);
	while ((n = read(from, buf, sizeof(buf))) != 0) {
		if (n == -1) {
			if (errno == EINTR)
				continue;
			err(1, "read self");
		}
		written = 0;
		while (written < n) {
			ssize_t rv = write(to, buf + written, n - written);
			if (rv == -1) {
				if (errno == EINTR)
					continue;
				err(1, "write executable");
			}
			if (rv == 0)
				errx(1, "write executable made no progress");
			written += rv;
		}
	}
	if (close(from) == -1 || close(to) == -1)
		err(1, "close copied executable");
}

static void
test_exec(void)
{
	char path[PATH_MAX];
	pid_t pid;

	if (selfpath[0] == '\0')
		errx(1, "cannot resolve path to test executable");
	make_path(path, sizeof(path), "exec");
	(void)unlink(path);
	copy_self(path);
	pid = fork();
	if (pid == -1)
		err(1, "fork exec");
	if (pid == 0) {
		execl(path, path, "-E", (char *)NULL);
		warn("execl: %s", path);
		_exit(126);
	}
	wait_ok(pid, "execute mapped ZFS image");
	if (unlink(path) == -1)
		err(1, "unlink executable");
}

static void
set_nonblock(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL);
	if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		err(1, "fcntl O_NONBLOCK");
}

static void
test_write_race(void)
{
	unsigned char *buf;
	volatile unsigned char *map;
	char path[PATH_MAX], stop;
	size_t len, off;
	unsigned int i;
	int fd, pipefd[2];
	pid_t pid;

	len = pagesize * 4;
	buf = malloc(pagesize);
	if (buf == NULL)
		err(1, "malloc");
	memset(buf, 0x55, pagesize);
	fd = make_file("write_race", len);
	for (off = 0; off < len; off += pagesize)
		write_at(fd, buf, pagesize, off);
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap write_race");
	if (pipe(pipefd) == -1)
		err(1, "pipe write_race");
	set_nonblock(pipefd[0]);

	pid = fork();
	if (pid == -1)
		err(1, "fork write_race");
	if (pid == 0) {
		close(pipefd[1]);
		for (;;) {
			ssize_t n = read(pipefd[0], &stop, 1);

			if (n == 1 || n == 0)
				_exit(0);
			if (n == -1 && errno != EAGAIN && errno != EINTR)
				_exit(3);
			for (off = 0; off < len; off += pagesize) {
				unsigned char a = map[off];
				unsigned char b = map[off + pagesize / 2];
				unsigned char c = map[off + pagesize - 1];
				if ((a != 0x55 && a != 0xaa) ||
				    (b != 0x55 && b != 0xaa) ||
				    (c != 0x55 && c != 0xaa))
					_exit(2);
			}
		}
	}
	close(pipefd[0]);
	alarm(120);
	for (i = 0; i < iterations; i++) {
		memset(buf, (i & 1) ? 0x55 : 0xaa, pagesize);
		for (off = 0; off < len; off += pagesize)
			write_at(fd, buf, pagesize, off);
	}
	if (write(pipefd[1], "X", 1) != 1 && errno != EPIPE)
		err(1, "stop write_race child");
	close(pipefd[1]);
	wait_ok(pid, "write/fault race");
	alarm(0);

	if (munmap((void *)map, len) == -1)
		err(1, "munmap write_race");
	if (close(fd) == -1)
		err(1, "close write_race");
	make_path(path, sizeof(path), "write_race");
	if (unlink(path) == -1)
		err(1, "unlink write_race");
	free(buf);
}

static void
test_shared_write_race(void)
{
	unsigned char update[97], got[97];
	volatile unsigned char *map;
	char path[PATH_MAX], stop;
	size_t len, off;
	unsigned int i;
	int fd, pipefd[2];
	pid_t pid;

	len = pagesize * 4;
	fd = make_file("shared_write_race", len);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap shared_write_race");
	if (pipe(pipefd) == -1)
		err(1, "pipe shared_write_race");
	set_nonblock(pipefd[0]);

	pid = fork();
	if (pid == -1)
		err(1, "fork shared_write_race");
	if (pid == 0) {
		close(pipefd[1]);
		for (i = 0;; i++) {
			ssize_t n = read(pipefd[0], &stop, 1);

			if (n == 1 || n == 0)
				_exit(0);
			if (n == -1 && errno != EAGAIN && errno != EINTR)
				_exit(3);
			for (off = 0; off < len; off += pagesize)
				map[off] = (unsigned char)i;
			if (msync((void *)map, len, MS_SYNC) == -1)
				_exit(2);
		}
	}
	close(pipefd[0]);
	alarm(120);
	for (i = 0; i < iterations; i++) {
		memset(update, (i & 1) ? 0x35 : 0xca, sizeof(update));
		for (off = 0; off < len; off += pagesize)
			write_at(fd, update, sizeof(update), off + 113);
	}
	if (write(pipefd[1], "X", 1) != 1 && errno != EPIPE)
		err(1, "stop shared_write_race child");
	close(pipefd[1]);
	wait_ok(pid, "shared write/msync race");
	alarm(0);
	if (msync((void *)map, len, MS_SYNC) == -1)
		err(1, "final msync shared_write_race");
	for (off = 0; off < len; off += pagesize) {
		read_at(fd, got, sizeof(got), off + 113);
		check_bytes(got, update, sizeof(got),
		    "pwrite bytes after shared write race");
	}

	if (munmap((void *)map, len) == -1 || close(fd) == -1)
		err(1, "close shared_write_race");
	make_path(path, sizeof(path), "shared_write_race");
	if (unlink(path) == -1)
		err(1, "unlink shared_write_race");
}

static void
test_truncate_race(void)
{
	unsigned char byte = 1;
	volatile unsigned char *map;
	char path[PATH_MAX], stop;
	size_t len;
	unsigned int i;
	int fd, pipefd[2];
	pid_t pid;

	len = pagesize * 2;
	fd = make_file("truncate_race", len);
	map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap truncate_race");
	if (pipe(pipefd) == -1)
		err(1, "pipe truncate_race");
	set_nonblock(pipefd[0]);

	pid = fork();
	if (pid == -1)
		err(1, "fork truncate_race");
	if (pid == 0) {
		ssize_t rv;

		close(pipefd[1]);
		for (;;) {
			const struct iovec *iov = (const struct iovec *)map;
			ssize_t n = read(pipefd[0], &stop, 1);

			if (n == 1 || n == 0)
				_exit(0);
			if (n == -1 && errno != EAGAIN && errno != EINTR)
				_exit(3);
			/* Force copyin(9) to fault pages while the vnode changes. */
			rv = pwritev(-1, iov, 1, 0);
			(void)rv;
		}
	}
	close(pipefd[0]);
	alarm(120);
	for (i = 0; i < iterations; i++) {
		if (ftruncate(fd, 0) == -1)
			err(1, "truncate_race shrink");
		if (ftruncate(fd, len) == -1)
			err(1, "truncate_race grow");
		write_at(fd, &byte, 1, pagesize);
	}
	if (write(pipefd[1], "X", 1) != 1 && errno != EPIPE)
		err(1, "stop truncate_race child");
	close(pipefd[1]);
	wait_ok(pid, "truncate/fault race");
	alarm(0);

	if (munmap((void *)map, len) == -1)
		err(1, "munmap truncate_race");
	if (close(fd) == -1)
		err(1, "close truncate_race");
	make_path(path, sizeof(path), "truncate_race");
	if (unlink(path) == -1)
		err(1, "unlink truncate_race");
}

static void
usage(const char *prog)
{
	size_t i;

	fprintf(stderr, "usage: %s -d directory [-n iterations] [test]\n",
	    prog);
	fprintf(stderr, "tests:");
	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		fprintf(stderr, " %s", tests[i].name);
	fprintf(stderr, " all\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *selected = "all";
	const char *prog = argv[0];
	char *end;
	unsigned long value;
	size_t i;
	int ch, ran = 0;

	if (argc == 2 && strcmp(argv[1], "-E") == 0)
		return (0);
	if (realpath(argv[0], selfpath) == NULL)
		selfpath[0] = '\0';
	while ((ch = getopt(argc, argv, "d:n:")) != -1) {
		switch (ch) {
		case 'd':
			if (realpath(optarg, testdir) == NULL)
				err(1, "realpath: %s", optarg);
			break;
		case 'n':
			errno = 0;
			value = strtoul(optarg, &end, 10);
			if (errno != 0 || *optarg == '\0' || *end != '\0' ||
			    value == 0 || value > UINT_MAX)
				errx(1, "invalid iteration count: %s", optarg);
			iterations = value;
			break;
		default:
			usage(argv[0]);
		}
	}
	argc -= optind;
	argv += optind;
	if (testdir[0] == '\0' || argc > 1)
		usage(prog);
	if (argc == 1)
		selected = argv[0];
	pagesize = (size_t)sysconf(_SC_PAGESIZE);
	if (pagesize == 0 || pagesize == (size_t)-1)
		err(1, "sysconf _SC_PAGESIZE");
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		err(1, "signal SIGPIPE");
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (strcmp(selected, "all") != 0 &&
		    strcmp(selected, tests[i].name) != 0)
			continue;
		printf("%s: running\n", tests[i].name);
		tests[i].func();
		printf("%s: ok\n", tests[i].name);
		ran = 1;
	}
	if (!ran)
		usage(prog);
	return (0);
}
