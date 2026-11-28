// SPDX-License-Identifier: MPL-2.0

// Package monitortest constructs synthetic Core events for binding tests.
package monitortest

/*
#cgo CFLAGS: -I${SRCDIR}/../../../include
#include "zlink.h"
*/
import "C"

import "unsafe"

func WithEvent(event, value, id uint64, lane, flags uint32, use func(unsafe.Pointer)) {
	raw := C.zlink_socket_monitor_event_t{
		event:          C.uint64_t(event),
		value:          C.uint64_t(value),
		connection_id:  C.uint64_t(id),
		transport_lane: C.uint32_t(lane),
		flags:          C.uint32_t(flags),
	}
	use(unsafe.Pointer(&raw))
}
