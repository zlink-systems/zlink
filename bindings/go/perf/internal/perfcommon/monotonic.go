package perfcommon

/*
#ifdef _WIN32
#include <windows.h>
static long long zlink_perf_monotonic_ns(void) {
	LARGE_INTEGER counter;
	LARGE_INTEGER frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (long long)((counter.QuadPart / frequency.QuadPart) * 1000000000LL
		+ ((counter.QuadPart % frequency.QuadPart) * 1000000000LL) / frequency.QuadPart);
}
#else
#include <time.h>
static long long zlink_perf_monotonic_ns(void) {
	struct timespec value;
	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return -1;
	return (long long)value.tv_sec * 1000000000LL + value.tv_nsec;
}
#endif
*/
import "C"

// MonotonicNowNs uses the host-wide monotonic clock so separate perf
// processes can compare metric-header timestamps on the same host.
func MonotonicNowNs() int64 {
	now := int64(C.zlink_perf_monotonic_ns())
	if now < 0 {
		panic("failed to read the monotonic clock")
	}
	return now
}
