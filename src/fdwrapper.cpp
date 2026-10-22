// SPDX-License-Identifier: BSD-3-Clause

#include <atomic>
#include <mutex>
#include <new>
#include <string.h>
#include <unistd.h>

#include <android/log.h>

extern "C" {
#include <qcomtee_object_types.h>
}

#include "fdwrapper.h"

#define LOG_TAG "minkcompat"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct FdWrapper {
	std::atomic<int> refs;
	int fd;
	std::mutex lock;
	struct qcomtee_object *memobj;
};

int32_t fdwrapper_invoke(ObjectCxt cxt, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	FdWrapper *me = static_cast<FdWrapper *>(cxt);

	switch (ObjectOp_methodID(op)) {
	case Object_OP_retain:
		me->refs.fetch_add(1);
		return Object_OK;
	case Object_OP_release:
		if (me->refs.fetch_sub(1) == 1) {
			if (me->memobj)
				qcomtee_memory_object_release(me->memobj);
			close(me->fd);
			delete me;
		}
		return Object_OK;
	case Object_OP_unwrapFd:
		if (counts != ObjectCounts_pack(0, 1, 0, 0) ||
		    args[0].b.size < sizeof(me->fd))
			return Object_ERROR_INVALID;
		memcpy(args[0].b.ptr, &me->fd, sizeof(me->fd));
		args[0].b.size = sizeof(me->fd);
		return Object_OK;
	default:
		return Object_ERROR_INVALID;
	}
}

struct qcomtee_object *own_memory_object(FdWrapper *me, struct qcomtee_object *root,
					 int *err)
{
	std::lock_guard<std::mutex> guard(me->lock);

	if (!me->memobj &&
	    qcomtee_memory_object_register_fd(me->fd, root, &me->memobj)) {
		ALOGE("failed to register dma-buf fd %d with QTEE", me->fd);
		me->memobj = nullptr;
		*err = Object_ERROR_INVALID;
		return nullptr;
	}

	qcomtee_object_refs_inc(me->memobj);
	return me->memobj;
}

} // namespace

extern "C" Object minkcompat_fdwrapper_new(int fd)
{
	FdWrapper *me = new (std::nothrow) FdWrapper;

	if (!me)
		return Object_NULL;

	me->refs = 1;
	me->fd = fd;
	me->memobj = nullptr;

	return (Object){ fdwrapper_invoke, me };
}

extern "C" Object FdWrapper_new(int fd)
{
	return minkcompat_fdwrapper_new(fd);
}

/*
 * Called by libminkadaptor when it marshals an object for QTEE. Any object
 * that unwraps to a dma-buf fd becomes a memory object, including the
 * FdWrapper and MSMem objects of libminksocket_vendor. Our own FdWrapper
 * registers its dma-buf once and keeps the memory object.
 */
extern "C" struct qcomtee_object *
mink_adaptor_memory_object(Object obj, struct qcomtee_object *root, int *err)
{
	struct qcomtee_object *memobj;
	ObjectArg arg;
	int fd = -1;

	*err = Object_OK;
	if (obj.invoke == fdwrapper_invoke)
		return own_memory_object(static_cast<FdWrapper *>(obj.context), root, err);

	arg.b = (ObjectBuf){ &fd, sizeof(fd) };
	if (Object_invoke(obj, Object_OP_unwrapFd, &arg, ObjectCounts_pack(0, 1, 0, 0)) ||
	    fd < 0)
		return nullptr;

	if (qcomtee_memory_object_register_fd(fd, root, &memobj)) {
		ALOGE("failed to register dma-buf fd %d with QTEE", fd);
		*err = Object_ERROR_INVALID;
		return nullptr;
	}

	return memobj;
}
