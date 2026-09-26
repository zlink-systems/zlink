//go:build linux

// SPDX-License-Identifier: MPL-2.0

// Package completiontest supplies a socket-scoped native completion fixture.
// Only tests import it, so linker wrapping never affects a binding build.
package completiontest

/*
#cgo CFLAGS: -I../../../include
#cgo LDFLAGS: -Wl,--wrap=zlink_completion_recv -Wl,--wrap=zlink_send -Wl,--wrap=zlink_send_rid -Wl,--wrap=zlink_request -Wl,--wrap=zlink_ctx_shutdown -Wl,--wrap=zlink_ctx_term -Wl,--wrap=zlink_poller_wait -Wl,--wrap=zlink_poller_modify -Wl,--wrap=zlink_poll
#include <stdint.h>
#include <stdlib.h>
void fixture_start(void *socket);
void fixture_stop(void);
void fixture_watch_context(void *context);
void fixture_override_empty_poller_wait_once(void);
void fixture_override_missing_poller_modify_once(void);
void fixture_override_poll_failure_once(void);
void fixture_writable(uint64_t id, uintptr_t context, const char *rid);
void fixture_request(uint64_t id, uintptr_t context);
const char *fixture_trace(void);
*/
import "C"

import "unsafe"

func Start(socket unsafe.Pointer)         { C.fixture_start(socket) }
func Stop()                               { C.fixture_stop() }
func WatchContext(context unsafe.Pointer) { C.fixture_watch_context(context) }
func OverrideEmptyPollerWaitOnce()        { C.fixture_override_empty_poller_wait_once() }
func OverrideMissingPollerModifyOnce()    { C.fixture_override_missing_poller_modify_once() }
func OverridePollFailureOnce()            { C.fixture_override_poll_failure_once() }

func Writable(id uint64, context uintptr, rid string) {
	value := C.CString(rid)
	defer C.free(unsafe.Pointer(value))
	C.fixture_writable(C.uint64_t(id), C.uintptr_t(context), value)
}

func Request(id uint64, context uintptr) {
	C.fixture_request(C.uint64_t(id), C.uintptr_t(context))
}

func Trace() string { return C.GoString(C.fixture_trace()) }
