// SPDX-License-Identifier: BSD-3-Clause

#ifndef MINKCOMPAT_FDWRAPPER_H
#define MINKCOMPAT_FDWRAPPER_H

#include "object.h"

#ifdef __cplusplus
extern "C" {
#endif

Object FdWrapper_new(int fd);
int FdWrapper_fd(Object obj);

#ifdef __cplusplus
}
#endif

#endif
