package native

import (
	"syscall"
	"testing"
)

func TestNativePollerWaitCapturesErrorAtCallBoundary(t *testing.T) {
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	// A native argument failure must retain both result and errno. Reading
	// errno in a later cgo call is not valid when Go resumes on another thread.
	count, result, errno := nativePollerWait(poller.handle, nil, 0, 0)
	if count != -1 || result != ConfigInvalidArgument || errno != int(syscall.EINVAL) {
		t.Fatalf("native wait=(%d,%v,%d), want (-1,InvalidArgument,EINVAL)", count, result, errno)
	}
}
