//go:build linux

// SPDX-License-Identifier: MPL-2.0

// Package completiontest supplies a socket-scoped native completion fixture.
// Only tests import it, so linker wrapping never affects a binding build.
package completiontest

/*
#cgo CFLAGS: -I../../../include
#cgo LDFLAGS: -Wl,--wrap=zlink_completion_recv -Wl,--wrap=zlink_send -Wl,--wrap=zlink_send_rid -Wl,--wrap=zlink_request -Wl,--wrap=zlink_ctx_shutdown -Wl,--wrap=zlink_ctx_term -Wl,--wrap=zlink_poller_wait -Wl,--wrap=zlink_poller_modify -Wl,--wrap=zlink_poll -Wl,--wrap=zlink_close
#include <stdint.h>
#include <stdlib.h>
void fixture_start(void *socket);
void fixture_stop(void);
void fixture_watch_context(void *context);
void fixture_override_empty_poller_wait_once(void);
void fixture_override_missing_poller_modify_once(void);
void fixture_override_poll_failure_once(void);
void fixture_override_close_busy_once(void);
void fixture_early_send_wait_once(void);
int fixture_early_send_waiting(void);
void fixture_release_early_send(void);
void fixture_writable(uint64_t id, uintptr_t context, const char *rid);
void fixture_writable_result(uint64_t id, uintptr_t context, const char *rid, int result, int terminal_errno);
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
func OverrideCloseBusyOnce()              { C.fixture_override_close_busy_once() }
func EarlySendWaitOnce()                  { C.fixture_early_send_wait_once() }
func EarlySendWaiting() bool              { return C.fixture_early_send_waiting() != 0 }
func ReleaseEarlySend()                   { C.fixture_release_early_send() }

func Writable(id uint64, context uintptr, rid string) {
	value := C.CString(rid)
	defer C.free(unsafe.Pointer(value))
	C.fixture_writable(C.uint64_t(id), C.uintptr_t(context), value)
}

func WritableResult(id uint64, context uintptr, rid string, result, terminalErrno int) {
	value := C.CString(rid)
	defer C.free(unsafe.Pointer(value))
	C.fixture_writable_result(C.uint64_t(id), C.uintptr_t(context), value,
		C.int(result), C.int(terminalErrno))
}

func Request(id uint64, context uintptr) {
	C.fixture_request(C.uint64_t(id), C.uintptr_t(context))
}

func Trace() string { return C.GoString(C.fixture_trace()) }
