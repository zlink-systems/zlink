/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_H_INCLUDED__
#define __ZLINK_H_INCLUDED__

/*  Version macros are kept here so existing CMake/version checks that read
    zlink.h directly do not need to parse nested public headers. */
#define ZLINK_VERSION_MAJOR 11
#define ZLINK_VERSION_MINOR 2
#define ZLINK_VERSION_PATCH 0

#define ZLINK_MAKE_VERSION(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))
#define ZLINK_VERSION                                                                              \
    ZLINK_MAKE_VERSION (ZLINK_VERSION_MAJOR, ZLINK_VERSION_MINOR, ZLINK_VERSION_PATCH)

#include <zlink/common.h>
#include <zlink/core/api.h>
#include <zlink/message/api.h>
#include <zlink/socket/api.h>
#include <zlink/eventing/api.h>

#undef ZLINK_EXPORT

#endif
