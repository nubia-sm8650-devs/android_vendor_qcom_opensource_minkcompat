// SPDX-License-Identifier: BSD-3-Clause

#include <atomic>
#include <mutex>
#include <new>
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

	(void)args;
	(void)counts;

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
	default:
		return Object_ERROR_INVALID;
	}
}

} // namespace

extern "C" Object FdWrapper_new(int fd)
{
	FdWrapper *me = new (std::nothrow) FdWrapper;

	if (!me)
		return Object_NULL;

	me->refs = 1;
	me->fd = fd;
	me->memobj = nullptr;

	return (Object){ fdwrapper_invoke, me };
}

extern "C" int FdWrapper_fd(Object obj)
{
	if (obj.invoke != fdwrapper_invoke)
		return -1;

	return static_cast<FdWrapper *>(obj.context)->fd;
}

/*
 * Called by libminkadaptor when it marshals an object for QTEE. A dma-buf is
 * registered once per FdWrapper, so that passing the same buffer again does
 * not create a second SHM bridge over the same range.
 */
extern "C" struct qcomtee_object *
mink_adaptor_memory_object(Object obj, struct qcomtee_object *root, int *err)
{
	FdWrapper *me;

	*err = Object_OK;
	if (obj.invoke != fdwrapper_invoke)
		return nullptr;

	me = static_cast<FdWrapper *>(obj.context);
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
