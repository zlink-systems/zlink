package zlink_test

import (
	"bytes"
	"context"
	"errors"
	"runtime"
	"syscall"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestPublicSendRetriesExactPacketAfterWritable(t *testing.T) {
	ctx := newContext(t)
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
	if err := router.SetSendHighWaterMark(1); err != nil {
		t.Fatalf("router SetSendHighWaterMark(1) error = %v", err)
	}
	if err := dealer.SetReceiveHighWaterMark(1); err != nil {
		t.Fatalf("dealer SetReceiveHighWaterMark(1) error = %v", err)
	}

	dealerRID := zlink.NewRoutingID([]byte("go-public-writable-peer"))
	if err := dealer.SetRoutingID(dealerRID); err != nil {
		t.Fatalf("dealer SetRoutingID() error = %v", err)
	}
	endpoint := inprocEndpoint("public-writable-retry")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("router Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("dealer Connect() error = %v", err)
	}

	// This blocking exchange is the route-adoption barrier; no scheduling
	// delay is needed before the routed send.
	if err := dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background()); err != nil {
		t.Fatalf("dealer prime Submit() error = %v", err)
	}
	var prime zlink.Received
	if ok, err := router.Recv(&prime, zlink.RecvFlagsNone); err != nil || !ok {
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

	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	defer poller.Close()
	const pollSlot = uintptr(91)
	if err := poller.AddSocket(router, zlink.PollOut|zlink.PollCompletion, pollSlot); err != nil {
		t.Fatalf("AddSocket(PollOut|PollCompletion) error = %v", err)
	}

	retryPayload := bytes.Repeat([]byte{'r'}, 64)
	started := make(chan struct{})
	sendDone := make(chan error, 1)
	go func() {
		close(started)
		sendDone <- router.SendTo(dealerRID).Bytes(retryPayload).Submit(context.Background())
	}()
	<-started
	assertSendRemainsBackpressured(t, sendDone)

	events := make([]zlink.PollEvent, 1)
	if n, err := poller.Wait(events, 0); err != nil || n != 0 {
		t.Fatalf("Wait() before peer drain = (%d, %v), want (0, nil)", n, err)
	}

	assertReceivedPayload(t, dealer, filler)
	n, err := poller.Wait(events, 5*time.Second)
	if err != nil {
		t.Fatalf("Wait() after peer drain error = %v", err)
	}
	if n != 1 || events[0].Slot != pollSlot || events[0].Revents&zlink.PollOut == 0 {
		t.Fatalf("Wait() after peer drain = (%d, %+v), want one POLLOUT event", n, events[0])
	}
	if events[0].Revents&zlink.PollCompletion != 0 {
		t.Fatalf("WRITABLE retry leaked as a successful-SEND completion: %+v", events[0])
	}

	if err, ok := awaitSendResult(sendDone); !ok || err != nil {
		t.Fatalf("retried Submit() result = (%v, %v), want (nil, true)", err, ok)
	}
	assertReceivedPayload(t, dealer, retryPayload)
	var duplicate zlink.Received
	if ok, err := dealer.Recv(&duplicate, zlink.RecvFlagsDontWait); err != nil || ok {
		t.Fatalf("Recv(DontWait) after exact retry = (%v, %v), want no duplicate", ok, err)
	}
}

func TestPublicBackpressuredSendReportsRouteRemoval(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	if err := router.SetLinger(0); err != nil {
		t.Fatalf("router SetLinger(0) error = %v", err)
	}
	if err := dealer.SetLinger(0); err != nil {
		t.Fatalf("dealer SetLinger(0) error = %v", err)
	}
	if err := router.SetSendHighWaterMark(1); err != nil {
		t.Fatalf("router SetSendHighWaterMark(1) error = %v", err)
	}
	if err := dealer.SetReceiveHighWaterMark(1); err != nil {
		t.Fatalf("dealer SetReceiveHighWaterMark(1) error = %v", err)
	}

	dealerRID := zlink.NewRoutingID([]byte("go-public-terminal-peer"))
	if err := dealer.SetRoutingID(dealerRID); err != nil {
		t.Fatalf("dealer SetRoutingID() error = %v", err)
	}
	endpoint := inprocEndpoint("public-writable-terminal")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("router Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("dealer Connect() error = %v", err)
	}
	if err := dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background()); err != nil {
		t.Fatalf("dealer prime Submit() error = %v", err)
	}
	var prime zlink.Received
	if ok, err := router.Recv(&prime, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("router prime Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	_ = prime.Close()
	if err := router.SendTo(dealerRID).Bytes(bytes.Repeat([]byte{'f'}, 64)).Submit(context.Background()); err != nil {
		t.Fatalf("HWM filler Submit() error = %v", err)
	}

	sendDone := make(chan error, 1)
	started := make(chan struct{})
	go func() {
		close(started)
		sendDone <- router.SendTo(dealerRID).Bytes([]byte("must-not-send")).Submit(context.Background())
	}()
	<-started
	assertSendRemainsBackpressured(t, sendDone)
	if err := router.DisconnectRID(dealerRID); err != nil {
		t.Fatalf("DisconnectRID() error = %v", err)
	}

	select {
	case err := <-sendDone:
		var submitErr *zlink.SubmitError
		if !errors.As(err, &submitErr) || submitErr.Result != zlink.SubmitNotFound {
			t.Fatalf("terminal send error = %v, want SubmitNotFound", err)
		}
		if !errors.Is(err, syscall.ENOENT) {
			t.Fatalf("terminal send error = %v, want ENOENT", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("backpressured send remained blocked after route removal")
	}
}

func assertSendRemainsBackpressured(t testing.TB, done <-chan error) {
	t.Helper()
	for attempt := 0; attempt < 100_000; attempt++ {
		select {
		case err := <-done:
			t.Fatalf("backpressured Submit() completed before peer drain: %v", err)
		default:
			runtime.Gosched()
		}
	}
}

func awaitSendResult(done <-chan error) (error, bool) {
	for attempt := 0; attempt < 100_000; attempt++ {
		select {
		case err := <-done:
			return err, true
		default:
			runtime.Gosched()
		}
	}
	return nil, false
}

func assertReceivedPayload(t testing.TB, socket *zlink.DealerSocket, want []byte) {
	t.Helper()
	var received zlink.Received
	if ok, err := socket.Recv(&received, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("dealer Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	defer received.Close()
	part, err := received.SinglePartOrError()
	if err != nil || !bytes.Equal(part.Data(), want) {
		t.Fatalf("received payload = (%q, %v), want %q", partDataForTest(part), err, want)
	}
}

func partDataForTest(part *zlink.Message) []byte {
	if part == nil {
		return nil
	}
	return part.Data()
}
