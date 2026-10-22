// SPDX-License-Identifier: BSD-3-Clause

#include <pthread.h>

#include <android/log.h>

#include "MinkCom.h"
#include "fdwrapper.h"
#include "object.h"

#define LOG_TAG "minkcompat"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static Object root_env = Object_NULL;

static int get_root(Object *obj)
{
	int ret = Object_OK;

	if (!obj)
		return Object_ERROR;

	pthread_mutex_lock(&root_lock);
	if (Object_isNull(root_env)) {
		ret = MinkCom_getRootEnvObject(&root_env);
		if (ret) {
			ALOGE("MinkCom_getRootEnvObject failed: %d", ret);
			root_env = Object_NULL;
		}
	}
	if (!ret) {
		Object_retain(root_env);
		*obj = root_env;
	}
	pthread_mutex_unlock(&root_lock);

	return ret;
}

extern "C" int TZCom_getRootEnvObject(Object *obj)
{
	return get_root(obj);
}

extern "C" int TZCom_getRootEnvObjectWithCB(size_t cbthread_cnt, size_t cbbuf_len, Object *obj)
{
	(void)cbthread_cnt;
	(void)cbbuf_len;
	return get_root(obj);
}

extern "C" int MinkDescriptor_getRootEnv(size_t cbthread_cnt, size_t cbbuf_len, Object *obj)
{
	(void)cbthread_cnt;
	(void)cbbuf_len;
	return get_root(obj);
}

extern "C" int TZCom_getClientEnvObject(Object *obj)
{
	Object root = Object_NULL;
	int ret;

	ret = get_root(&root);
	if (ret)
		return -1;

	ret = MinkCom_getClientEnvObject(root, obj);
	if (ret)
		ALOGE("MinkCom_getClientEnvObject failed: %d", ret);

	Object_release(root);
	return ret;
}

extern "C" int TZCom_getFdObject(int fd, Object *obj)
{
	if (fd < 0 || !obj) {
		ALOGE("invalid fd %d or NULL object pointer", fd);
		return Object_ERROR;
	}

	*obj = minkcompat_fdwrapper_new(fd);
	if (Object_isNull(*obj))
		return Object_ERROR_KMEM;

	return Object_OK;
}
