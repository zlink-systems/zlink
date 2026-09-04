package native

import (
	"bytes"
	"context"
	"fmt"
	"runtime"
	"testing"
	"time"
)

func TestManagedSendRetriesExactPacketAfterWritable(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	router, err := ctx.RouterSocket()
	if err != nil {
		t.Fatalf("RouterSocket() error = %v", err)
	}
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	if err != nil {
		t.Fatalf("DealerSocket() error = %v", err)
	}
	defer dealer.Close()
	if err := router.SetLinger(0); err != nil {
		t.Fatalf("router SetLinger(0) error = %v", err)
	}
	if err := dealer.SetLinger(0); err != nil {
		t.Fatalf("dealer SetLinger(0) error = %v", err)
	}
	// A one-byte HWM admits one oversized complete record into an empty pipe,
	// then deterministically backpressures the next record until the peer drains.
	if err := router.SetSendHighWaterMark(1); err != nil {
		t.Fatalf("router SetSendHighWaterMark(1) error = %v", err)
	}
	if err := dealer.SetReceiveHighWaterMark(1); err != nil {
		t.Fatalf("dealer SetReceiveHighWaterMark(1) error = %v", err)
	}

	dealerRID := NewRoutingIDString("go-writable-retry-peer")
	if err := dealer.SetRoutingID(dealerRID); err != nil {
		t.Fatalf("dealer SetRoutingID() error = %v", err)
	}
	endpoint := fmt.Sprintf("inproc://go-writable-retry-%p", ctx)
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("router Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("dealer Connect() error = %v", err)
	}

	// The blocking prime synchronizes route adoption without a sleep.
	if err := dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background()); err != nil {
		t.Fatalf("dealer prime Submit() error = %v", err)
	}
	var prime Received
	if ok, err := router.Recv(&prime, RecvFlagsNone); err != nil || !ok {
		t.Fatalf("router prime Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	if !prime.RoutingID().Equal(dealerRID) {
		t.Fatalf("prime routing id = %v, want %v", prime.RoutingID(), dealerRID)
	}
	_ = prime.Close()

	filler := bytes.Repeat([]byte{'f'}, 64)
	if err := router.SendTo(dealerRID).Bytes(filler).Submit(context.Background()); err != nil {
		t.Fatalf("HWM filler Submit() error = %v", err)
	}

	poller, err := NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	defer poller.Close()
	const pollSlot = uintptr(73)
	if err := poller.AddSocket(router, PollOut|PollCompletion, pollSlot); err != nil {
		t.Fatalf("AddSocket(PollOut|PollCompletion) error = %v", err)
	}

	retryPayload := bytes.Repeat([]byte{'r'}, 64)
	sendDone := make(chan error, 1)
	go func() {
		sendDone <- router.SendTo(dealerRID).Bytes(retryPayload).Submit(context.Background())
	}()

	entry, waitToken := waitForManagedSendToken(t, router.socketCore.completion)
	if waitToken == 0 {
		t.Fatal("backpressured send published a zero WRITABLE wait token")
	}
	if entry.handleKey == 0 || entry.send == nil || !entry.send.hasTarget || !entry.send.target.Equal(dealerRID) {
		t.Fatalf("wait entry context/target = (%d, %v), want nonzero context and %v", entry.handleKey, entry.send, dealerRID)
	}
	if len(entry.send.payload.owned) != 1 || !bytes.Equal(entry.send.payload.owned[0].Data(), retryPayload) {
		t.Fatal("managed send did not retain the exact logical packet for retry")
	}

	events := make([]PollEvent, 1)
	if n, err := poller.Wait(events, 0); err != nil || n != 0 {
		t.Fatalf("Wait() before peer drain = (%d, %v), want (0, nil)", n, err)
	}
	select {
	case err := <-sendDone:
		t.Fatalf("backpressured Submit() completed before peer drain: %v", err)
	default:
	}

	var received Received
	if ok, err := dealer.Recv(&received, RecvFlagsNone); err != nil || !ok {
		t.Fatalf("dealer filler Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	part, err := received.SinglePartOrError()
	if err != nil || !bytes.Equal(part.Data(), filler) {
		t.Fatalf("filler payload = (%q, %v), want exact filler", partData(part), err)
	}
	_ = received.Close()

	n, err := poller.Wait(events, 5*time.Second)
	if err != nil {
		t.Fatalf("Wait() after peer drain error = %v", err)
	}
	if n != 1 || events[0].Slot != pollSlot || events[0].Revents&PollOut == 0 {
		t.Fatalf("Wait() after peer drain = (%d, %+v), want one POLLOUT event", n, events[0])
	}
	if events[0].Revents&PollCompletion != 0 {
		t.Fatalf("WRITABLE-only wake leaked as successful SEND completion: %+v", events[0])
	}
	entry.waitSettled()
	entry.mu.Lock()
	settledErr := entry.err
	settledToken := entry.completion
	entry.mu.Unlock()
	if settledErr != nil || settledToken != 0 {
		t.Fatalf("retry settlement = (token %d, error %v), want (0, nil); initial token was %d", settledToken, settledErr, waitToken)
	}
	if err, ok := waitForSendResult(sendDone); !ok || err != nil {
		t.Fatalf("retried Submit() result = (%v, %v), want (nil, true)", err, ok)
	}

	var retried Received
	if ok, err := dealer.Recv(&retried, RecvFlagsNone); err != nil || !ok {
		t.Fatalf("dealer retry Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	retriedPart, err := retried.SinglePartOrError()
	if err != nil || !bytes.Equal(retriedPart.Data(), retryPayload) {
		t.Fatalf("retried payload = (%q, %v), want exact retained packet", partData(retriedPart), err)
	}
	_ = retried.Close()

	var duplicate Received
	if ok, err := dealer.Recv(&duplicate, RecvFlagsDontWait); err != nil || ok {
		t.Fatalf("Recv(DontWait) after exact retry = (%v, %v), want no duplicate", ok, err)
	}
}

func waitForManagedSendToken(t testing.TB, owner *completionOwner) (*completionEntry, uint64) {
	t.Helper()
	for attempt := 0; attempt < 100_000; attempt++ {
		var waiting *completionEntry
		owner.mu.Lock()
		for _, entry := range owner.entries {
			if entry.kind == completionSendRetry && entry.writableWaiting {
				waiting = entry
				break
			}
		}
		if waiting != nil {
			waiting.mu.Lock()
			published := waiting.published
			token := waiting.completion
			waiting.mu.Unlock()
			owner.mu.Unlock()
			if published && token != 0 {
				return waiting, token
			}
			runtime.Gosched()
			continue
		}
		owner.mu.Unlock()
		runtime.Gosched()
	}
	t.Fatal("managed send did not reach a backpressured WRITABLE wait")
	return nil, 0
}

func waitForSendResult(done <-chan error) (error, bool) {
	for attempt := 0; attempt < 10_000; attempt++ {
		select {
		case err := <-done:
			return err, true
		default:
			runtime.Gosched()
		}
	}
	return nil, false
}

func partData(part *Message) []byte {
	if part == nil {
		return nil
	}
	return part.Data()
}
