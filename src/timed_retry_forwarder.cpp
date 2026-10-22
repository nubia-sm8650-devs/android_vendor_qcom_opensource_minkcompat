// SPDX-License-Identifier: BSD-3-Clause

#include <atomic>
#include <mutex>
#include <new>
#include <time.h>

#include "object.h"

namespace {

struct TimedRetryForwarder {
	std::atomic<int> refs;
	size_t ms_timeout;
	size_t ms_interval;
	Object obj;
	std::mutex lock;
};

int32_t forwarder_new(size_t ms_timeout, size_t ms_interval, Object obj, Object *out);

long long now_ms()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(size_t ms)
{
	struct timespec ts = {
		.tv_sec = (time_t)(ms / 1000),
		.tv_nsec = (long)(ms % 1000) * 1000000,
	};

	nanosleep(&ts, nullptr);
}

int32_t forwarder_invoke(ObjectCxt cxt, ObjectOp op, ObjectArg *args, ObjectCounts counts)
{
	TimedRetryForwarder *me = static_cast<TimedRetryForwarder *>(cxt);
	long long deadline;
	int32_t ret;

	if (ObjectOp_isLocal(op)) {
		switch (ObjectOp_methodID(op)) {
		case Object_OP_retain:
			me->refs.fetch_add(1);
			return Object_OK;
		case Object_OP_release:
			if (me->refs.fetch_sub(1) == 1) {
				Object_release(me->obj);
				delete me;
			}
			return Object_OK;
		default:
			return Object_ERROR_REMOTE;
		}
	}

	std::lock_guard<std::mutex> guard(me->lock);

	deadline = now_ms() + (long long)me->ms_timeout;
	for (;;) {
		ret = Object_invoke(me->obj, op, args, counts);
		if (ret != Object_ERROR_BUSY || now_ms() >= deadline)
			break;
		sleep_ms(me->ms_interval);
	}

	if (!Object_isOK(ret))
		return ret;

	size_t first = ObjectCounts_indexOO(counts);
	size_t last = first + ObjectCounts_numOO(counts);

	for (size_t i = first; i < last; i++) {
		Object wrapped = Object_NULL;

		if (Object_isNull(args[i].o) || args[i].o.invoke != me->obj.invoke)
			continue;

		ret = forwarder_new(me->ms_timeout, me->ms_interval, args[i].o, &wrapped);
		if (!Object_isOK(ret)) {
			for (size_t j = first; j < last; j++) {
				if (!Object_isNull(args[j].o))
					Object_release(args[j].o);
				args[j].o = Object_NULL;
			}
			return ret;
		}
		args[i].o = wrapped;
	}

	return Object_OK;
}

int32_t forwarder_new(size_t ms_timeout, size_t ms_interval, Object obj, Object *out)
{
	TimedRetryForwarder *me = new (std::nothrow) TimedRetryForwarder;

	if (!me)
		return Object_ERROR_KMEM;

	me->refs = 1;
	me->ms_timeout = ms_timeout;
	me->ms_interval = ms_interval;
	me->obj = obj;
	*out = (Object){ forwarder_invoke, me };

	return Object_OK;
}

} // namespace

int32_t TimedRetryForwarder_new(size_t msTimeout, size_t msInterval, Object obj, Object *objOut)
{
	return forwarder_new(msTimeout, msInterval, obj, objOut);
}
