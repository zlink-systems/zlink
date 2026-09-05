package main

import (
	"context"
	"sync"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type barrierRequest struct {
	arrived chan<- struct{}
	release <-chan struct{}
	payload []byte
}

func (r *barrierRequest) Message(*zlink.Message) zlink.RequestSubmitOp { return r }
func (r *barrierRequest) Bytes(data []byte) zlink.RequestSubmitOp {
	if r.payload == nil {
		r.payload = data
	}
	return r
}
func (r *barrierRequest) Timeout(time.Duration) zlink.RequestSubmitOp { return r }
func (r *barrierRequest) Submit(context.Context) ([]*zlink.Message, error) {
	r.arrived <- struct{}{}
	<-r.release
	body, err := zlink.NewMessage(r.payload)
	if err != nil {
		return nil, err
	}
	tail, err := zlink.NewMessageWithSize(0)
	if err != nil {
		body.Close()
		return nil, err
	}
	return []*zlink.Message{body, tail}, nil
}

func TestReqRepSocketsSubmitConcurrentlyAndExcludeLateReplies(t *testing.T) {
	t.Setenv("PERF_PART_COUNT", "2")
	const count = 8
	arrived := make(chan struct{}, count)
	release := make(chan struct{})
	var releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })
	clients := make([]multiReqRepClient, count)
	for i := range clients {
		clients[i].request = func() zlink.RequestOp { return &barrierRequest{arrived: arrived, release: release} }
	}
	cfg := multiConfig{pattern: "MULTI_DEALER_ROUTER_REQREP", transport: "tcp", clients: count, msgSize: 64, duration: 100 * time.Millisecond}
	window := activeDeadline(cfg.duration)
	done := make(chan perfcommon.Result, 1)
	go func() { done <- runMultiReqRepWindow(cfg, clients, window, time.Second) }()
	deadline := time.NewTimer(5 * time.Second)
	defer deadline.Stop()
	for i := 0; i < count; i++ {
		select {
		case <-arrived:
		case <-deadline.C:
			t.Fatalf("only %d/%d sockets submitted before any reply was released", i, count)
		}
	}
	// All terminals are in flight. Return their replies after the measurement
	// deadline; a completed request in the drain must not inflate throughput.
	if wait := time.Until(window.StopAt); wait > 0 {
		time.Sleep(wait)
	}
	releaseOnce.Do(func() { close(release) })
	select {
	case result := <-done:
		if result.Valid || result.Throughput != 0 {
			t.Fatalf("late replies counted: %+v", result)
		}
	case <-deadline.C:
		t.Fatal("workers did not join after reply completion")
	}
}
