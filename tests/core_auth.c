/*
 * Copyright 2015 David Herrmann <dh.herrmann@gmail.com>
 * Copyright 2018 Collabora, Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * Testcase: drmGetMagic() and drmAuthMagic()
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/sysmacros.h>
#include "drm.h"

#ifdef __linux__
# include <sys/syscall.h>
#else
# include <pthread.h>
#endif

IGT_TEST_DESCRIPTION("Call drmGetMagic() and drmAuthMagic() and see if it behaves.");

static bool
is_local_tid(pid_t tid)
{
#ifndef __linux__
	return pthread_self() == tid;
#else
	/* On Linux systems, drmGetClient() would return the thread ID
	   instead of the actual process ID */
	return syscall(SYS_gettid) == tid;
#endif
}


static bool check_auth(int fd)
{
	pid_t client_pid;
	int i, auth, pid, uid;
	unsigned long magic, iocs;
	bool is_authenticated = false;

	client_pid = getpid();
	for (i = 0; !is_authenticated; i++) {
		if (drmGetClient(fd, i, &auth, &pid, &uid, &magic, &iocs) != 0)
			break;
		is_authenticated = auth && (pid == client_pid || is_local_tid(pid));
	}
	return is_authenticated;
}


static int magic_cmp(const void *p1, const void *p2)
{
	return *(const drm_magic_t*)p1 < *(const drm_magic_t*)p2;
}

static void test_many_magics(int master)
{
	drm_magic_t magic, *magics = NULL;
	unsigned int i, j, ns, allocated = 0;
	char path[512];
	int *fds = NULL, slave;

	struct rlimit fd_limit;

	do_or_die(getrlimit(RLIMIT_NOFILE, &fd_limit));
	fd_limit.rlim_cur = 1024;
	do_or_die(setrlimit(RLIMIT_NOFILE, &fd_limit));

	sprintf(path, "/proc/self/fd/%d", master);

	for (i = 0; ; ++i) {
		/* open slave and make sure it's NOT a master */
		slave = open(path, O_RDWR | O_CLOEXEC);
		if (slave < 0) {
			igt_info("Reopening device failed after %d opens\n", i);
			igt_assert(errno == EMFILE);
			break;
		}
		igt_assert(drmSetMaster(slave) < 0);

		/* resize magic-map */
		if (i >= allocated) {
			ns = allocated * 2;
			igt_assert(ns >= allocated);

			if (!ns)
				ns = 128;

			magics = realloc(magics, sizeof(*magics) * ns);
			igt_assert(magics);

			fds = realloc(fds, sizeof(*fds) * ns);
			igt_assert(fds);

			allocated = ns;
		}

		/* insert magic */
		igt_assert(drmGetMagic(slave, &magic) == 0);
		igt_assert(magic > 0);

		magics[i] = magic;
		fds[i] = slave;
	}

	/* make sure we could at least open a reasonable number of files */
	igt_assert(i > 128);

	/*
	 * We cannot open the DRM file anymore. Lets sort the magic-map and
	 * verify no magic was used multiple times.
	 */
	qsort(magics, i, sizeof(*magics), magic_cmp);
	for (j = 1; j < i; ++j)
		igt_assert(magics[j] != magics[j - 1]);

	/* make sure we can authenticate all of them */
	for (j = 0; j < i; ++j)
		igt_assert(drmAuthMagic(master, magics[j]) == 0);

	/* close files again */
	for (j = 0; j < i; ++j)
		close(fds[j]);

	free(fds);
	free(magics);
}

static void test_basic_auth(int master)
{
	drm_magic_t magic, old_magic;
	int slave;

	/* open slave and make sure it's NOT a master */
	slave = drm_open_driver(DRIVER_ANY);
	igt_require(slave >= 0);
	igt_require(drmSetMaster(slave) < 0);

	/* retrieve magic for slave */
	igt_assert(drmGetMagic(slave, &magic) == 0);
	igt_assert(magic > 0);

	/* verify the same magic is returned every time */
	old_magic = magic;
	igt_assert(drmGetMagic(slave, &magic) == 0);
	igt_assert_eq(magic, old_magic);

	/* verify magic can be authorized exactly once, on the master */
	igt_assert(drmAuthMagic(slave, magic) < 0);
	igt_assert(drmAuthMagic(master, magic) == 0);
	igt_assert(drmAuthMagic(master, magic) < 0);

	/* verify that the magic did not change */
	old_magic = magic;
	igt_assert(drmGetMagic(slave, &magic) == 0);
	igt_assert_eq(magic, old_magic);

	close(slave);
}

static bool has_prime_import(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_IMPORT;
}

static void check_auth_sanity(int master)
{
	uint32_t handle;

	igt_assert(check_auth(master) == true);
	igt_require(has_prime_import(master));

	igt_assert(drmPrimeFDToHandle(master, -1, &handle) < 0);

	/* IOCTL requires authenticated master as done in drm_permit.
	 * As we get past that, we'll fail due to the invalid FD.
	 *
	 * Note: strictly speaking this is unrelated to the goal of
	 * the test, although danvet requested it.
	 */
	igt_assert(errno == EBADF);
}

static bool has_render_node(int fd)
{
	char node_name[80];
	struct stat sbuf;

	if (fstat(fd, &sbuf))
		return false;

	sprintf(node_name, "/dev/dri/renderD%d", minor(sbuf.st_rdev) | 0x80);
	if (stat(node_name, &sbuf))
		return false;

	return true;
}

/*
 * Testcase: Render capable, unauthenticated master doesn't throw -EACCES for
 * DRM_RENDER_ALLOW ioctls.
 */
static void test_unauth_vs_render(int master)
{
	int slave;
	uint32_t handle;
	struct stat statbuf;
	bool has_render;

	/* need to check for render nodes before we wreak the filesystem */
	has_render = has_render_node(master);

	/* create a card node matching master which (only) we can access as
	 * non-root */
	do_or_die(fstat(master, &statbuf));
	do_or_die(unshare(CLONE_NEWNS));
	do_or_die(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL));
	do_or_die(mount("none", "/dev/dri", "tmpfs", 0, NULL));
	umask(0);
	do_or_die(mknod("/dev/dri/card", S_IFCHR | 0666, statbuf.st_rdev));

	igt_drop_root();

	slave = open("/dev/dri/card", O_RDWR);

	igt_assert(slave >= 0);

	/*
	 * The second open() happens without CAP_SYS_ADMIN, thus it will NOT
	 * be authenticated.
	 */
	igt_assert(check_auth(slave) == false);

	/* Issuing the following ioctl will fail, no doubt about it. */
	igt_assert(drmPrimeFDToHandle(slave, -1, &handle) < 0);

	/*
	 * Updated kernels allow render capable, unauthenticated master to
	 * issue DRM_AUTH ioctls (like FD2HANDLE above), as long as they are
	 * annotated as DRM_RENDER_ALLOW.
	 *
	 * Otherwise, errno is set to -EACCES
	 *
	 * Note: We are _not_ interested in the FD2HANDLE specific errno,
	 * yet the EBADF check is added on the explicit request by danvet.
	 */
	if (has_render)
		igt_assert(errno == EBADF);
	else
		igt_assert(errno == EACCES);

	close(slave);
}

igt_main
{
	int master;

	/* root (which we run igt as) should always be authenticated */
	igt_subtest("getclient-simple") {
		int fd = drm_open_driver(DRIVER_ANY);

		igt_assert(check_auth(fd) == true);

		close(fd);
	}

	igt_subtest("getclient-master-drop") {
		int fd = drm_open_driver(DRIVER_ANY);
		int fd2 = drm_open_driver(DRIVER_ANY);

		igt_assert(check_auth(fd2) == true);

		close(fd);

		igt_assert(check_auth(fd2) == true);

		close(fd2);
	}

	/* above tests require that no drm fd is open */
	igt_subtest_group {
		igt_fixture
			master = drm_open_driver_master(DRIVER_ANY);

		igt_subtest("basic-auth")
			test_basic_auth(master);

		/* this must be last, we adjust the rlimit */
		igt_subtest("many-magics")
			test_many_magics(master);
	}

	igt_subtest_group {
		igt_fixture
			master = drm_open_driver(DRIVER_ANY);

		igt_subtest("unauth-vs-render") {
			check_auth_sanity(master);

			igt_fork(child, 1)
				test_unauth_vs_render(master);
			igt_waitchildren();
		}
	}
}
