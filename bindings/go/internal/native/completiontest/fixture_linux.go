//go:build linux

// SPDX-License-Identifier: MPL-2.0

// Package completiontest supplies a socket-scoped native completion fixture.
// Only tests import it, so linker wrapping never affects a binding build.
package completiontest

/*
#cgo CFLAGS: -I../../../include
#cgo LDFLAGS: -Wl,--wrap=zlink_completion_recv -Wl,--wrap=zlink_send_part -Wl,--wrap=zlink_send_part_rid -Wl,--wrap=zlink_request_part
#include <stdint.h>
#include <stdlib.h>
void fixture_start(void *socket);
void fixture_stop(void);
void fixture_writable(uint64_t id, uintptr_t context, const char *rid);
void fixture_request(uint64_t id, uintptr_t context);
const char *fixture_trace(void);
*/
import "C"

import "unsafe"

func Start(socket unsafe.Pointer) { C.fixture_start(socket) }
func Stop()                       { C.fixture_stop() }

func Writable(id uint64, context uintptr, rid string) {
	value := C.CString(rid)
	defer C.free(unsafe.Pointer(value))
	C.fixture_writable(C.uint64_t(id), C.uintptr_t(context), value)
}

func Request(id uint64, context uintptr) {
	C.fixture_request(C.uint64_t(id), C.uintptr_t(context))
}

func Trace() string { return C.GoString(C.fixture_trace()) }
