// SPDX-License-Identifier: BSD-3-Clause

#ifndef MINKCOMPAT_FDWRAPPER_H
#define MINKCOMPAT_FDWRAPPER_H

#include "object.h"

#ifndef Object_OP_unwrapFd
#define Object_OP_unwrapFd (ObjectOp_METHOD_MASK - 2)
#endif

#ifdef __cplusplus
extern "C" {
#endif

Object FdWrapper_new(int fd);

/* Not exported, so that TZCom_getFdObject() never binds to another library's FdWrapper_new(). */
__attribute__((visibility("hidden"))) Object minkcompat_fdwrapper_new(int fd);

#ifdef __cplusplus
}
#endif

#endif
