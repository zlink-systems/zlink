/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_COMMON_H_INCLUDED
#define ZLINK_COMMON_H_INCLUDED

/*  Version macros for compile-time API version detection                     */
#ifndef ZLINK_VERSION_MAJOR
#define ZLINK_VERSION_MAJOR 11
#endif
#ifndef ZLINK_VERSION_MINOR
#define ZLINK_VERSION_MINOR 1
#endif
#ifndef ZLINK_VERSION_PATCH
#define ZLINK_VERSION_PATCH 0
#endif

#ifndef ZLINK_MAKE_VERSION
#define ZLINK_MAKE_VERSION(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))
#endif
#ifndef ZLINK_VERSION
#define ZLINK_VERSION                                                                              \
    ZLINK_MAKE_VERSION (ZLINK_VERSION_MAJOR, ZLINK_VERSION_MINOR, ZLINK_VERSION_PATCH)
#endif

#include <stddef.h>
#include <stdio.h>

#if !defined(__cplusplus)
#include <stdbool.h>
#endif

/*  Handle DSO symbol visibility                                             */
#if defined ZLINK_NO_EXPORT
#define ZLINK_EXPORT
#else
#if defined _WIN32
#if defined ZLINK_STATIC
#define ZLINK_EXPORT
#elif defined DLL_EXPORT
#define ZLINK_EXPORT __declspec (dllexport)
#else
#define ZLINK_EXPORT __declspec (dllimport)
#endif
#else
#if defined __SUNPRO_C || defined __SUNPRO_CC
#define ZLINK_EXPORT __global
#elif (defined __GNUC__ && __GNUC__ >= 4) || defined __INTEL_COMPILER
#define ZLINK_EXPORT __attribute__ ((visibility ("default")))
#else
#define ZLINK_EXPORT
#endif
#endif
#endif

/*  Define integer types needed for event interface                          */
#define ZLINK_DEFINED_STDINT 1
#if defined ZLINK_HAVE_SOLARIS || defined ZLINK_HAVE_OPENVMS
#include <inttypes.h>
#elif defined _MSC_VER && _MSC_VER < 1600
#ifndef uint64_t
typedef unsigned __int64 uint64_t;
#endif
#ifndef int32_t
typedef __int32 int32_t;
#endif
#ifndef uint32_t
typedef unsigned __int32 uint32_t;
#endif
#ifndef uint16_t
typedef unsigned __int16 uint16_t;
#endif
#ifndef uint8_t
typedef unsigned __int8 uint8_t;
#endif
#else
#include <stdint.h>
#endif

#if !defined _WIN32
#include <signal.h>
#endif

#ifdef ZLINK_HAVE_AIX
#include <poll.h>
#endif

#include <zlink_enum.h>
#include <zlink_errno.h>

#endif
