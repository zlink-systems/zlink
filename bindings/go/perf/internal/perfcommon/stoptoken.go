// Wire-level stop token used by sender goroutines to signal phase end to
// receivers waiting on a poller. PERF_SINGLE_TEST_POLICY § 1.4 and
// PERF_MULTI_TEST_POLICY § 1.3.1 mandate this pattern instead of an
// atomic flag / cancellation channel + short polling fallback.

package perfcommon

import (
	"bytes"
	"time"

	zlink "zlink.systems/zlink/v11"
)

// StopToken is the literal payload a sender writes once at phase end. The
// receiver exits when it observes a message whose only part matches the
// token.
var StopToken = []byte("__zlink_perf_stop__")

// IsStopToken reports whether the given byte slice equals the wire-level
// stop token.
func IsStopToken(b []byte) bool {
	return bytes.Equal(b, StopToken)
}

// IsStopTokenMessage reports whether a single zlink.Message matches the
// stop token. Returns false for nil parts.
func IsStopTokenMessage(part *zlink.Message) bool {
	if part == nil {
		return false
	}
	return IsStopToken(part.Data())
}

// StopTokenSendBackoff is the initial backoff a blocking-stop-token send
// uses when the socket reports transient backpressure. The wait is
// bounded by StopTokenSendAttempts so the sender goroutine cannot hang
// indefinitely on a fully blocked peer. Attempts are sized for ~5 s of
// drain time which covers heavy multi-pattern fan-out workloads.
const (
	StopTokenSendBackoff  = time.Millisecond
	StopTokenSendAttempts = 5000
)
