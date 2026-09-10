package perfcommon

import "time"

var (
	monotonicOrigin       = time.Now()
	monotonicOriginUnixNs = monotonicOrigin.UnixNano()
)

// MonotonicNowNs anchors Go's process-local monotonic clock to the host wall
// clock once, keeping returned values comparable between local perf processes
// without importing a native implementation into perf code.
func MonotonicNowNs() int64 {
	return monotonicOriginUnixNs + time.Since(monotonicOrigin).Nanoseconds()
}
