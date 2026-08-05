// SPDX-License-Identifier: MPL-2.0

package native

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo linux,amd64 LDFLAGS: -L${SRCDIR}/../../native/linux-x86_64 -lzlink -Wl,-rpath,${SRCDIR}/../../native/linux-x86_64
#cgo linux,arm64 LDFLAGS: -L${SRCDIR}/../../native/linux-aarch64 -lzlink -Wl,-rpath,${SRCDIR}/../../native/linux-aarch64
#cgo darwin,amd64 LDFLAGS: -L${SRCDIR}/../../native/darwin-x86_64 -lzlink -Wl,-rpath,${SRCDIR}/../../native/darwin-x86_64
#cgo darwin,arm64 LDFLAGS: -L${SRCDIR}/../../native/darwin-aarch64 -lzlink -Wl,-rpath,${SRCDIR}/../../native/darwin-aarch64
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.h"

extern void goZlinkRecvTrampoline(zlink_routing_id_t *source_rid_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);
extern void goZlinkSendReadyTrampoline(void *subject_, uintptr_t userdata_);
extern void goZlinkMonitorTrampoline(zlink_monitor_event_t *event_, uintptr_t userdata_);
extern void goZlinkReplyTrampoline(zlink_request_result_t result_, zlink_msg_t *parts_, size_t part_count_, uintptr_t userdata_);
*/
import "C"
