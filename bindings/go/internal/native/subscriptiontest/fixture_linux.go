//go:build linux

// SPDX-License-Identifier: MPL-2.0

package subscriptiontest

/*
#cgo CFLAGS: -I../../../include
#cgo LDFLAGS: -Wl,--wrap=zlink_subscription_at
#include <errno.h>
#include "zlink.h"

static void *watched_socket;
static size_t calls;
static size_t capacities[2];
static zlink_config_result_t results[2];
static int retry_result;

zlink_config_result_t __real_zlink_subscription_at(void *, size_t, char *, size_t *, int *);

zlink_config_result_t __wrap_zlink_subscription_at(
    void *socket, size_t index, char *filter, size_t *size, int *pattern)
{
    if (socket != watched_socket)
        return __real_zlink_subscription_at(socket, index, filter, size, pattern);

    size_t call = calls++;
    if (call < 2)
        capacities[call] = filter ? *size : 0;
    zlink_config_result_t result;
    if (retry_result == ZLINK_CONFIG_INVALID_ARGUMENT) {
        *size = 4096;
        errno = EINVAL;
        result = ZLINK_CONFIG_INVALID_ARGUMENT;
    } else if (call == 1 && retry_result == ZLINK_CONFIG_NOT_FOUND) {
        errno = ENOENT;
        result = ZLINK_CONFIG_NOT_FOUND;
    } else {
        result = __real_zlink_subscription_at(socket, index, filter, size, pattern);
    }
    if (call < 2)
        results[call] = result;
    return result;
}

static void fixture_start(void *socket, int result)
{
    watched_socket = socket;
    calls = 0;
    retry_result = result;
}
static size_t fixture_calls(void) { return calls; }
static size_t fixture_capacity(int call) { return capacities[call]; }
static zlink_config_result_t fixture_result(int call) { return results[call]; }
*/
import "C"

import "unsafe"

func Start(socket unsafe.Pointer, result int) { C.fixture_start(socket, C.int(result)) }
func Stop()                                   { C.fixture_start(nil, 0) }
func Calls() int                              { return int(C.fixture_calls()) }
func Capacity(call int) int                   { return int(C.fixture_capacity(C.int(call))) }
func Result(call int) int                     { return int(C.fixture_result(C.int(call))) }
