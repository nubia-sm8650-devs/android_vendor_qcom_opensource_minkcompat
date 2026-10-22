// SPDX-License-Identifier: BSD-3-Clause

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "object.h"

extern "C" int TZCom_getClientEnvObject(Object *obj);
int32_t TimedRetryForwarder_new(size_t msTimeout, size_t msInterval, Object obj, Object *objOut);

#define IClientEnv_OP_open 0
#define CDiagnostics_UID 143
#define IDiagnostics_OP_queryHeapInfo 0

struct heap_info {
	uint32_t total_size;
	uint32_t used_size;
	uint32_t free_size;
	uint32_t overhead_size;
	uint32_t wasted_size;
	uint32_t largest_free_block_size;
};

static int32_t client_env_open(Object env, uint32_t uid, Object *out)
{
	ObjectArg a[2];

	a[0].bi = (ObjectBufIn){ &uid, sizeof(uid) };
	a[1].o = Object_NULL;
	int32_t ret = Object_invoke(env, IClientEnv_OP_open, a, ObjectCounts_pack(1, 0, 0, 1));
	*out = a[1].o;
	return ret;
}

int main(int argc, char **argv)
{
	uint32_t uid = argc > 1 ? strtoul(argv[1], NULL, 0) : CDiagnostics_UID;
	Object env = Object_NULL, wrapped = Object_NULL, svc = Object_NULL;
	struct heap_info hi = {};
	ObjectArg a[1];
	int32_t ret;

	ret = TZCom_getClientEnvObject(&env);
	printf("TZCom_getClientEnvObject: %d\n", ret);
	if (ret)
		return 1;

	ret = TimedRetryForwarder_new(2000, 30, env, &wrapped);
	printf("TimedRetryForwarder_new: %d\n", ret);
	if (ret)
		return 1;

	ret = client_env_open(wrapped, uid, &svc);
	printf("IClientEnv_open(%u): %d\n", uid, ret);
	if (ret)
		goto out;

	if (uid == CDiagnostics_UID) {
		a[0].b = (ObjectBuf){ &hi, sizeof(hi) };
		ret = Object_invoke(svc, IDiagnostics_OP_queryHeapInfo, a, ObjectCounts_pack(0, 1, 0, 0));
		printf("IDiagnostics_queryHeapInfo: %d total %u used %u free %u largest_free %u\n",
		       ret, hi.total_size, hi.used_size, hi.free_size, hi.largest_free_block_size);
	}

	Object_release(svc);
out:
	Object_release(wrapped);
	return ret ? 1 : 0;
}
