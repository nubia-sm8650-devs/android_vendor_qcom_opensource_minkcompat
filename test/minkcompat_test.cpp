// SPDX-License-Identifier: BSD-3-Clause

#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <linux/dma-heap.h>

#include "object.h"

#ifndef Object_OP_unwrapFd
#define Object_OP_unwrapFd (ObjectOp_METHOD_MASK - 2)
#endif

extern "C" int TZCom_getClientEnvObject(Object *obj);
extern "C" int TZCom_getFdObject(int fd, Object *obj);
int32_t TimedRetryForwarder_new(size_t msTimeout, size_t msInterval, Object obj, Object *objOut);

#define IClientEnv_OP_open 0
#define CDiagnostics_UID 143
#define IDiagnostics_OP_queryHeapInfo 0

/* An op QTEE does not implement, so that only argument marshalling runs. */
#define OP_UNKNOWN 0x7ffe

#define QSEECOM_HEAP "/dev/dma_heap/qcom,qseecom"

struct heap_info {
	uint32_t total_size;
	uint32_t used_size;
	uint32_t free_size;
	uint32_t overhead_size;
	uint32_t wasted_size;
	uint32_t largest_free_block_size;
};

static int failures;

static void check(bool ok, const char *what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

static int32_t client_env_open(Object env, uint32_t uid, Object *out)
{
	ObjectArg a[2];

	a[0].bi = (ObjectBufIn){ &uid, sizeof(uid) };
	a[1].o = Object_NULL;
	int32_t ret = Object_invoke(env, IClientEnv_OP_open, a, ObjectCounts_pack(1, 0, 0, 1));
	*out = a[1].o;
	return ret;
}

static int32_t query_heap_info(Object diag, struct heap_info *hi)
{
	ObjectArg a[1];

	a[0].b = (ObjectBuf){ hi, sizeof(*hi) };
	return Object_invoke(diag, IDiagnostics_OP_queryHeapInfo, a, ObjectCounts_pack(0, 1, 0, 0));
}

static int count_tee_shm_fds(void)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *de;
	char path[64], link[64];
	int count = 0;

	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		ssize_t len;

		snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
		len = readlink(path, link, sizeof(link) - 1);
		if (len <= 0)
			continue;
		link[len] = '\0';
		if (!strcmp(link, "anon_inode:tee_shm"))
			count++;
	}

	closedir(dir);
	return count;
}

static bool fd_is_open(int fd)
{
	return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static int alloc_dmabuf(size_t size)
{
	struct dma_heap_allocation_data alloc = {};
	int heap, ret;

	heap = open(QSEECOM_HEAP, O_RDONLY | O_CLOEXEC);
	if (heap < 0)
		return -1;

	alloc.len = size;
	alloc.fd_flags = O_RDWR | O_CLOEXEC;
	ret = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);
	close(heap);

	return ret ? -1 : (int)alloc.fd;
}

static int32_t pass_objects(Object env, Object *objs, size_t num)
{
	ObjectArg a[ObjectCounts_maxOI];

	for (size_t i = 0; i < num; i++)
		a[i].o = objs[i];

	return Object_invoke(env, OP_UNKNOWN, a, ObjectCounts_pack(0, 0, num, 0));
}

/* Stands in for the FdWrapper and MSMem objects of libminksocket_vendor. */
struct ForeignFd {
	std::atomic<int> refs;
	int fd;
};

static int32_t foreign_invoke(ObjectCxt cxt, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	ForeignFd *me = static_cast<ForeignFd *>(cxt);

	switch (ObjectOp_methodID(op)) {
	case Object_OP_retain:
		me->refs.fetch_add(1);
		return Object_OK;
	case Object_OP_release:
		if (me->refs.fetch_sub(1) == 1)
			delete me;
		return Object_OK;
	case Object_OP_unwrapFd:
		if (counts != ObjectCounts_pack(0, 1, 0, 0) || args[0].b.size < sizeof(int))
			return Object_ERROR_INVALID;
		memcpy(args[0].b.ptr, &me->fd, sizeof(int));
		return Object_OK;
	default:
		return Object_ERROR_INVALID;
	}
}

static void test_own_fdwrapper(Object env, size_t size, int base)
{
	Object mem = Object_NULL;
	ObjectArg a[1];
	int fd, unwrapped = -1;

	fd = alloc_dmabuf(size);
	check(fd >= 0, "allocate a qcom,qseecom dma-buf");
	if (fd < 0)
		return;

	if (TZCom_getFdObject(fd, &mem)) {
		check(false, "TZCom_getFdObject");
		close(fd);
		return;
	}

	a[0].b = (ObjectBuf){ &unwrapped, sizeof(unwrapped) };
	check(!Object_invoke(mem, Object_OP_unwrapFd, a, ObjectCounts_pack(0, 1, 0, 0)) &&
		      unwrapped == fd,
	      "FdWrapper unwraps to its fd");

	for (int i = 0; i < 2; i++)
		pass_objects(env, &mem, 1);
	check(count_tee_shm_fds() == base + 1, "FdWrapper registers its dma-buf once");

	Object_release(mem);
	check(count_tee_shm_fds() == base, "FdWrapper release drops the registration");
	check(!fd_is_open(fd), "FdWrapper release closes the dma-buf fd");
}

static void test_dup_fdwrappers(Object env, size_t size, int base)
{
	Object mem[2] = { Object_NULL, Object_NULL };
	int fd;

	fd = alloc_dmabuf(size);
	if (fd < 0)
		return;

	if (TZCom_getFdObject(fd, &mem[0]) || TZCom_getFdObject(dup(fd), &mem[1])) {
		check(false, "TZCom_getFdObject on a dup'd fd");
		if (Object_isNull(mem[0]))
			close(fd);
		Object_ASSIGN_NULL(mem[0]);
		return;
	}

	pass_objects(env, mem, 2);
	check(count_tee_shm_fds() == base + 2, "two FdWrappers over one dma-buf in one call");

	Object_release(mem[0]);
	Object_release(mem[1]);
	check(count_tee_shm_fds() == base, "both registrations are dropped");
}

static void test_foreign_wrapper(Object env, size_t size, int base)
{
	ForeignFd *foreign = new ForeignFd;
	Object obj;

	foreign->refs = 1;
	foreign->fd = alloc_dmabuf(size);
	if (foreign->fd < 0) {
		delete foreign;
		return;
	}

	obj = (Object){ foreign_invoke, foreign };
	pass_objects(env, &obj, 1);
	check(count_tee_shm_fds() == base, "foreign wrapper registration is dropped after the call");
	check(foreign->refs == 1, "foreign wrapper keeps its references");

	close(foreign->fd);
	Object_release(obj);
}

int main(int argc, char **argv)
{
	uint32_t uid = argc > 1 ? strtoul(argv[1], NULL, 0) : CDiagnostics_UID;
	Object env = Object_NULL, wrapped = Object_NULL, svc = Object_NULL;
	struct heap_info hi = {};
	int32_t ret;

	ret = TZCom_getClientEnvObject(&env);
	check(!ret, "TZCom_getClientEnvObject");
	if (ret)
		return 1;

	if (!access(QSEECOM_HEAP, R_OK)) {
		int base = count_tee_shm_fds();

		test_own_fdwrapper(env, 0x10000, base);
		test_dup_fdwrappers(env, 0x10000, base);
		test_foreign_wrapper(env, 0x10000, base);
	} else {
		printf("SKIP: no %s\n", QSEECOM_HEAP);
	}

	Object_retain(env);
	ret = TimedRetryForwarder_new(2000, 30, env, &wrapped);
	check(!ret, "TimedRetryForwarder_new");
	if (ret) {
		Object_release(env);
		goto out;
	}

	ret = client_env_open(wrapped, uid, &svc);
	printf("IClientEnv_open(%u): %d\n", uid, ret);
	check(!ret && !Object_isNull(svc), "IClientEnv_open");
	if (ret || uid != CDiagnostics_UID)
		goto out_svc;

	ret = query_heap_info(svc, &hi);
	printf("heap: total %u used %u free %u largest_free %u\n",
	       hi.total_size, hi.used_size, hi.free_size, hi.largest_free_block_size);
	check(!ret && hi.total_size && hi.free_size <= hi.total_size &&
		      hi.largest_free_block_size <= hi.free_size,
	      "IDiagnostics_queryHeapInfo");

	{
		std::atomic<int> errors{ 0 };
		std::vector<std::thread> threads;

		for (int t = 0; t < 4; t++)
			threads.emplace_back([&] {
				for (int i = 0; i < 50; i++) {
					struct heap_info h = {};

					if (query_heap_info(svc, &h) || !h.total_size)
						errors++;
				}
			});
		for (auto &thread : threads)
			thread.join();
		check(!errors, "concurrent calls through one forwarder");
	}

out_svc:
	Object_ASSIGN_NULL(svc);
	Object_release(wrapped);
out:
	Object_release(env);
	printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
