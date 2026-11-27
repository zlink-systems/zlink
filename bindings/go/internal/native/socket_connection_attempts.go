// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"unsafe"

	"zlink.systems/zlink/contracts"
)

type ConnectionAttempt = contracts.ConnectionAttempt

// ConnectionAttemptsSnapshot returns one row per outbound connection
// registration, with the endpoint owned by the returned strings. A snapshot
// that saw every change clears PollConnect readiness. Keep one connection
// observer per socket. A row Core cannot describe within the closed enums is
// a binding protocol violation, never a successful snapshot.
func (s *socketCore) ConnectionAttemptsSnapshot() ([]ConnectionAttempt, error) {
	rows := make([]C.zlink_connection_attempt_t, 16)
	endpoints := make([]byte, 1024)
	for {
		var count, bytes C.size_t
		rc, cerr := C.zlink_socket_connection_attempts_snapshot(
			s.raw(), &rows[0], C.size_t(len(rows)), &count,
			(*C.char)(unsafe.Pointer(&endpoints[0])), C.size_t(len(endpoints)), &bytes)
		if ConfigResult(rc) == ConfigBufferTooSmall {
			// Core keeps PollConnect readiness on this result; retry with the
			// sizes it reported (the set can grow again before the retry).
			if int(count) <= len(rows) && int(bytes) <= len(endpoints) {
				return nil, newConfigError(ConfigInternalError, 0)
			}
			if int(count) > len(rows) {
				rows = make([]C.zlink_connection_attempt_t, int(count))
			}
			if int(bytes) > len(endpoints) {
				endpoints = make([]byte, int(bytes))
			}
			continue
		}
		if err := configErrorFromCall(rc, cerr); err != nil {
			return nil, err
		}
		if int(count) > len(rows) || int(bytes) > len(endpoints) {
			return nil, newConfigError(ConfigInternalError, 0)
		}
		attempts := make([]ConnectionAttempt, int(count))
		for i := range attempts {
			row := &rows[i]
			offset, length := int(row.endpoint_offset), int(row.endpoint_length)
			if row.intent_id == 0 || row.revision == 0 ||
				offset >= int(bytes) || length >= int(bytes)-offset || endpoints[offset+length] != 0 ||
				row.state > C.ZLINK_CONNECTION_ATTEMPT_FAILED ||
				row.last_failure.kind > C.ZLINK_CONNECTION_FAILURE_OTHER ||
				row.last_failure.stage > C.ZLINK_CONNECTION_STAGE_ESTABLISHED {
				return nil, newConfigError(ConfigInternalError, 0)
			}
			attempts[i] = ConnectionAttempt{
				Endpoint:      string(endpoints[offset : offset+length]),
				IntentID:      uint64(row.intent_id),
				Revision:      uint64(row.revision),
				LastFailureID: uint64(row.last_failure_id),
				State:         contracts.ConnectionAttemptState(row.state),
				LastFailure: contracts.ConnectionFailure{
					Kind:             contracts.ConnectionFailureKind(row.last_failure.kind),
					Stage:            contracts.ConnectionFailureStage(row.last_failure.stage),
					NormalizedErrno:  int32(row.last_failure.normalized_errno),
					DisconnectReason: uint32(row.last_failure.disconnect_reason),
				},
			}
		}
		return attempts, nil
	}
}
