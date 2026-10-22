// SPDX-License-Identifier: BSD-3-Clause

#include <atomic>
#include <new>
#include <unistd.h>

#include "fdwrapper.h"

namespace {

struct FdWrapper {
	std::atomic<int> refs;
	int fd;
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

	return (Object){ fdwrapper_invoke, me };
}

extern "C" int FdWrapper_fd(Object obj)
{
	if (obj.invoke != fdwrapper_invoke)
		return -1;

	return static_cast<FdWrapper *>(obj.context)->fd;
}
