package zlink_test

import (
	"runtime"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

func TestNilLifecycleReceiversReturnTypedErrors(t *testing.T) {
	var options *zlink.ContextOptions
	if err := options.SetMaxSockets(1); err == nil {
		t.Fatal("nil ContextOptions.SetMaxSockets() should fail")
	}
	if _, err := options.MaxSockets(); err == nil {
		t.Fatal("nil ContextOptions.MaxSockets() should fail")
	}

	var monitor *zlink.SocketMonitor
	if _, err := monitor.Recv(zlink.RecvFlagsDontWait); err == nil {
		t.Fatal("nil SocketMonitor.Recv() should fail")
	}
	if _, err := monitor.Status(); err == nil {
		t.Fatal("nil SocketMonitor.Status() should fail")
	}
	if err := monitor.OnEvent(func(*zlink.MonitorEvent) {}); err == nil {
		t.Fatal("nil SocketMonitor.OnEvent() should fail")
	}
}

func TestThreadJoinConcurrentCallersWaitForCompletion(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	thread, err := zlink.NewThread(func() {
		close(started)
		<-release
	})
	if err != nil {
		t.Fatalf("NewThread() error = %v", err)
	}

	firstDone := make(chan error, 1)
	go func() { firstDone <- thread.Join() }()
	<-started
	time.Sleep(10 * time.Millisecond)
	secondDone := make(chan error, 1)
	go func() { secondDone <- thread.Join() }()
	select {
	case err := <-secondDone:
		t.Fatalf("concurrent Join() returned before the thread completed: %v", err)
	case <-time.After(20 * time.Millisecond):
	}

	close(release)
	if err := <-firstDone; err != nil {
		t.Fatalf("first Join() error = %v", err)
	}
	if err := <-secondDone; err != nil {
		t.Fatalf("second Join() error = %v", err)
	}
}

func TestSocketCloseAndOptionAccessHaveNoGoStateRace(t *testing.T) {
	ctx := newContext(t)
	socket, err := ctx.PairSocket()
	if err != nil {
		ctx.Close()
		t.Fatalf("PairSocket() error = %v", err)
	}

	done := make(chan struct{})
	go func() {
		defer close(done)
		for i := 0; i < 64; i++ {
			_ = socket.SetSendHighWaterMark(i + 1)
			runtime.Gosched()
		}
	}()
	_ = socket.Close()
	<-done
	_ = ctx.Close()
}
