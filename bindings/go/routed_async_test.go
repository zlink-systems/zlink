package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"syscall"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

// This file covers the routed send/request contract for the 0.13.1
// realignment: routed send is a SYNCHRONOUS Submit(ctx) error, the HWM wait
// lives inside Core (bounded by SNDTIMEO), and the binding owns no thread,
// queue or retry. Request keeps its completion channel, but the submit itself
// is synchronous and Core's reply callback drives the completion.

const routedAsyncTestHWM = 4096

type routedAsyncFixture struct {
	ctx     *zlink.Context
	router  *zlink.RouterSocket
	dealerA *zlink.DealerSocket
	dealerB *zlink.DealerSocket
	ridA    zlink.RoutingID
	ridB    zlink.RoutingID
}

func newRoutedAsyncFixture(t testing.TB, withB bool) *routedAsyncFixture {
	t.Helper()
	ctx := newContext(t)
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		ctx.Close()
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}
	router, err := ctx.RouterSocket()
	if err != nil {
		ctx.Close()
		t.Fatalf("RouterSocket() error = %v", err)
	}
	dealerA, err := ctx.DealerSocket()
	if err != nil {
		router.Close()
		ctx.Close()
		t.Fatalf("DealerSocket(A) error = %v", err)
	}
	f := &routedAsyncFixture{
		ctx:     ctx,
		router:  router,
		dealerA: dealerA,
		ridA:    zlink.NewRoutingID([]byte("routed-async-A")),
		ridB:    zlink.NewRoutingID([]byte("routed-async-B")),
	}
	if withB {
		f.dealerB, err = ctx.DealerSocket()
		if err != nil {
			f.close()
			t.Fatalf("DealerSocket(B) error = %v", err)
		}
	}

	for name, socket := range map[string]interface {
		SetLinger(time.Duration) error
	}{"router": f.router, "dealer A": f.dealerA} {
		if err := socket.SetLinger(0); err != nil {
			f.close()
			t.Fatalf("%s SetLinger(0) error = %v", name, err)
		}
	}
	if f.dealerB != nil {
		if err := f.dealerB.SetLinger(0); err != nil {
			f.close()
			t.Fatalf("dealer B SetLinger(0) error = %v", err)
		}
	}
	if err := f.router.SetMandatory(true); err != nil {
		f.close()
		t.Fatalf("router SetMandatory(true) error = %v", err)
	}
	if err := f.router.SetSendHighWaterMark(routedAsyncTestHWM); err != nil {
		f.close()
		t.Fatalf("router SetSendHighWaterMark() error = %v", err)
	}
	// The realigned contract puts the HWM wait inside Core, so every test
	// bounds it with the socket's own SNDTIMEO instead of a binding deadline.
	if err := f.router.SetSendTimeout(2 * time.Second); err != nil {
		f.close()
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}
	for name, dealer := range map[string]*zlink.DealerSocket{"A": f.dealerA, "B": f.dealerB} {
		if dealer == nil {
			continue
		}
		if err := dealer.SetRoutingID(map[string]zlink.RoutingID{"A": f.ridA, "B": f.ridB}[name]); err != nil {
			f.close()
			t.Fatalf("dealer %s SetRoutingID() error = %v", name, err)
		}
		if err := dealer.SetReceiveHighWaterMark(routedAsyncTestHWM); err != nil {
			f.close()
			t.Fatalf("dealer %s SetReceiveHighWaterMark() error = %v", name, err)
		}
		if err := dealer.SetReceiveTimeout(2 * time.Second); err != nil {
			f.close()
			t.Fatalf("dealer %s SetReceiveTimeout() error = %v", name, err)
		}
	}

	endpointA := inprocEndpoint("routed-async-a")
	if err := f.router.Bind(endpointA); err != nil {
		f.close()
		t.Fatalf("router Bind(A) error = %v", err)
	}
	if err := f.dealerA.Connect(endpointA); err != nil {
		f.close()
		t.Fatalf("dealer A Connect() error = %v", err)
	}
	waitForRoutedAsyncPeer(t, f.router, f.dealerA, f.ridA)

	if f.dealerB != nil {
		endpointB := inprocEndpoint("routed-async-b")
		if err := f.router.Bind(endpointB); err != nil {
			f.close()
			t.Fatalf("router Bind(B) error = %v", err)
		}
		if err := f.dealerB.Connect(endpointB); err != nil {
			f.close()
			t.Fatalf("dealer B Connect() error = %v", err)
		}
		waitForRoutedAsyncPeer(t, f.router, f.dealerB, f.ridB)
	}
	return f
}

func (f *routedAsyncFixture) close() {
	if f == nil {
		return
	}
	if f.router != nil {
		_ = f.router.Close()
	}
	if f.dealerA != nil {
		_ = f.dealerA.Close()
	}
	if f.dealerB != nil {
		_ = f.dealerB.Close()
	}
	if f.ctx != nil {
		_ = f.ctx.Close()
	}
}

func waitForRoutedAsyncPeer(t testing.TB, router *zlink.RouterSocket, dealer *zlink.DealerSocket, rid zlink.RoutingID) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		err := router.SendTo(rid).Bytes([]byte("route-ready")).Submit(context.Background())
		if err == nil {
			var received zlink.Received
			if ok, recvErr := dealer.Recv(&received, zlink.RecvFlagsNone); recvErr != nil || !ok {
				t.Fatalf("route-ready Recv() = (%v, %v), want (true, nil)", ok, recvErr)
			}
			_ = received.Close()
			return
		}
		var submitError *zlink.SubmitError
		if !errors.As(err, &submitError) || (submitError.Result != zlink.SubmitNotFound && submitError.Result != zlink.SubmitNotConnected) {
			t.Fatalf("route-ready send error = %v", err)
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("timed out waiting for routed peer")
}

// fillRoutedTargetUntilBackpressured drives one exact target to its Core HWM
// using SNDTIMEO=0 (the DONTWAIT contract). It returns how many records were
// admitted so the caller can drain exactly that many.
func fillRoutedTargetUntilBackpressured(t testing.TB, router *zlink.RouterSocket, rid zlink.RoutingID) int {
	t.Helper()
	if err := router.SetSendTimeout(0); err != nil {
		t.Fatalf("router SetSendTimeout(0) error = %v", err)
	}
	payload := make([]byte, routedAsyncTestHWM)
	for admitted := 0; admitted < 512; admitted++ {
		err := router.SendTo(rid).Bytes(payload).Submit(context.Background())
		if err == nil {
			continue
		}
		var submitError *zlink.SubmitError
		if !errors.As(err, &submitError) || submitError.Result != zlink.SubmitBackpressured {
			t.Fatalf("fill send error = %v, want SubmitBackpressured", err)
		}
		return admitted
	}
	t.Fatal("target did not reach HWM")
	return 0
}

func requireRequestCompletionClosed(t testing.TB, completion <-chan zlink.RequestReplyCompletion) {
	t.Helper()
	select {
	case _, ok := <-completion:
		if ok {
			t.Fatal("request completion produced more than one terminal result")
		}
	case <-time.After(time.Second):
		t.Fatal("request completion did not close after its terminal result")
	}
}

func recvRoutedAsync(t testing.TB, dealer *zlink.DealerSocket) *zlink.Received {
	t.Helper()
	received := &zlink.Received{}
	if ok, err := dealer.Recv(received, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("Dealer Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	return received
}

func drainRoutedAsync(t testing.TB, dealer *zlink.DealerSocket, count int) {
	t.Helper()
	for i := 0; i < count; i++ {
		received := recvRoutedAsync(t, dealer)
		_ = received.Close()
	}
}

// A backpressured exact target must not poison another target: the routed send
// terminal is synchronous, so B's submit returns while A is still full.
func TestRoutedSendBlockedTargetDoesNotPoisonOtherTarget(t *testing.T) {
	f := newRoutedAsyncFixture(t, true)
	defer f.close()

	admitted := fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	if err := f.router.SetSendTimeout(2 * time.Second); err != nil {
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}

	started := time.Now()
	if err := f.router.SendTo(f.ridB).Bytes([]byte("b-progress")).Submit(context.Background()); err != nil {
		t.Fatalf("target B send error = %v", err)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("target B send waited %s behind target A", elapsed)
	}
	receivedB := recvRoutedAsync(t, f.dealerB)
	if len(receivedB.Parts()) != 1 || string(receivedB.Parts()[0].Data()) != "b-progress" {
		t.Fatalf("target B payload was not preserved")
	}
	_ = receivedB.Close()

	drainRoutedAsync(t, f.dealerA, admitted)
	if err := f.router.SendTo(f.ridA).Bytes([]byte("a-after-drain")).Submit(context.Background()); err != nil {
		t.Fatalf("target A send after drain error = %v", err)
	}
	received := recvRoutedAsync(t, f.dealerA)
	_ = received.Close()
}

// SNDTIMEO=0 is the DONTWAIT contract: Core returns BACKPRESSURED at once and
// the binding does not retry.
func TestRoutedSendZeroSendTimeoutReturnsBackpressureImmediately(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	_ = fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	started := time.Now()
	err := f.router.SendTo(f.ridA).Bytes([]byte("zero-timeout")).Submit(context.Background())
	if err == nil {
		t.Fatal("zero-timeout routed send completed without backpressure")
	}
	var submitError *zlink.SubmitError
	if !errors.As(err, &submitError) || submitError.Result != zlink.SubmitBackpressured {
		t.Fatalf("zero-timeout routed send error = %v, want SubmitBackpressured", err)
	}
	if elapsed := time.Since(started); elapsed >= time.Second {
		t.Fatalf("zero-timeout routed send waited %s behind the target queue", elapsed)
	}
}

// Builder flags select the synchronous non-blocking terminal independently of
// the socket timeout. DONTWAIT must surface Core backpressure immediately.
func TestRoutedSendBuilderDontWaitReturnsBackpressureImmediately(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	_ = fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	if err := f.router.SetSendTimeout(2 * time.Second); err != nil {
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}

	started := time.Now()
	err := f.router.SendTo(f.ridA).
		Bytes([]byte("builder-dontwait")).
		Flags(zlink.SendFlagsDontWait).
		Submit(context.Background())
	if err == nil {
		t.Fatal("DONTWAIT routed send completed without backpressure")
	}
	var submitError *zlink.SubmitError
	if !errors.As(err, &submitError) || submitError.Result != zlink.SubmitBackpressured {
		t.Fatalf("DONTWAIT routed send error = %v, want SubmitBackpressured", err)
	}
	if elapsed := time.Since(started); elapsed >= time.Second {
		t.Fatalf("DONTWAIT routed send waited %s behind the target queue", elapsed)
	}
}

// The blocking submit parks inside Core and resumes on a Core credit signal.
// Nothing in the binding retries or reschedules it.
func TestRoutedSendBlockingSubmitParksInCoreAndResumesOnCredit(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	admitted := fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	if err := f.router.SetSendTimeout(5 * time.Second); err != nil {
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}

	done := make(chan error, 1)
	go func() {
		done <- f.router.SendTo(f.ridA).Bytes([]byte("parked-in-core")).Submit(context.Background())
	}()

	select {
	case err := <-done:
		t.Fatalf("blocked send returned before credit came back: %v", err)
	case <-time.After(150 * time.Millisecond):
	}

	drainRoutedAsync(t, f.dealerA, admitted)
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("send after Core credit signal error = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("blocked send did not resume after its credit returned")
	}
}

// SNDTIMEO bounds the Core-owned wait; expiry surfaces as BACKPRESSURED.
func TestRoutedSendSendTimeoutBoundsTheCoreOwnedWait(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	_ = fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	if err := f.router.SetSendTimeout(200 * time.Millisecond); err != nil {
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}
	started := time.Now()
	err := f.router.SendTo(f.ridA).Bytes([]byte("bounded-wait")).Submit(context.Background())
	elapsed := time.Since(started)
	if err == nil {
		t.Fatal("bounded routed send completed without backpressure")
	}
	var submitError *zlink.SubmitError
	if !errors.As(err, &submitError) || submitError.Result != zlink.SubmitBackpressured {
		t.Fatalf("bounded routed send error = %v, want SubmitBackpressured", err)
	}
	if elapsed < 100*time.Millisecond {
		t.Fatalf("bounded routed send returned after %s, want at least the SNDTIMEO", elapsed)
	}
	if elapsed > 3*time.Second {
		t.Fatalf("bounded routed send waited %s past its SNDTIMEO", elapsed)
	}
}

// A route that goes away surfaces a terminal submit error, not a retry.
func TestRoutedSendDetachedTargetSurfacesTerminalError(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	if err := f.dealerA.Close(); err != nil {
		t.Fatalf("dealer A Close() error = %v", err)
	}
	f.dealerA = nil

	var lastErr error
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		lastErr = f.router.SendTo(f.ridA).Bytes([]byte("detached")).Submit(context.Background())
		if lastErr != nil {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if lastErr == nil {
		t.Fatal("send to a detached target never surfaced an error")
	}
	var submitError *zlink.SubmitError
	if !errors.As(lastErr, &submitError) {
		t.Fatalf("detached target error = %v, want *SubmitError", lastErr)
	}
}

// ctx owns cancellation at the submit boundary: an already-cancelled ctx fails
// before anything reaches the wire.
func TestRoutedSendCancelledContextNeverReachesTheWire(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	err := f.router.SendTo(f.ridA).Bytes([]byte("must-not-reach-wire")).Submit(ctx)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled submit error = %v, want context.Canceled", err)
	}

	var received zlink.Received
	if ok, recvErr := f.dealerA.Recv(&received, zlink.RecvFlagsDontWait); recvErr != nil {
		t.Fatalf("drain Recv() error = %v", recvErr)
	} else if ok {
		payload := string(received.Parts()[0].Data())
		_ = received.Close()
		t.Fatalf("cancelled send reached the wire: %q", payload)
	}
}

func TestRoutedSendExpiredContextDeadlineNeverReachesTheWire(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	ctx, cancel := context.WithTimeout(context.Background(), time.Nanosecond)
	defer cancel()
	time.Sleep(time.Millisecond)
	err := f.router.SendTo(f.ridA).Bytes([]byte("expired")).Submit(ctx)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("expired submit error = %v, want context.DeadlineExceeded", err)
	}
}

// Concurrent multipart attempts are application-coordinated. The binding does
// not serialize them: Core either accepts a whole record or rejects the whole
// competing attempt with EINVAL. Binding-owned staging preserves the public
// Message values even though Core consumes each attempted native part.
func TestRoutedSendConcurrentMultipartBindingStagingPreservesParts(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()
	if err := f.router.SetSendHighWaterMark(1 << 20); err != nil {
		t.Fatalf("router SetSendHighWaterMark() error = %v", err)
	}
	if err := f.dealerA.SetReceiveHighWaterMark(1 << 20); err != nil {
		t.Fatalf("dealer A SetReceiveHighWaterMark() error = %v", err)
	}

	const senders = 8
	const perSender = 500

	var wg sync.WaitGroup
	type senderResult struct {
		successes int
		rejected  int
		err       error
	}
	results := make(chan senderResult, senders)
	for sender := 0; sender < senders; sender++ {
		wg.Add(1)
		go func(sender int) {
			defer wg.Done()
			result := senderResult{}
			for i := 0; i < perSender; i++ {
				prefix := fmt.Sprintf("record-%d-%03d", sender, i)
				first := newMessage(t, prefix+"-a")
				second := newMessage(t, prefix+"-b")
				err := f.router.SendTo(f.ridA).
					Message(first).
					Message(second).
					Submit(context.Background())
				if err == nil {
					result.successes++
					continue
				}
				var submitErr *zlink.SubmitError
				if !errors.As(err, &submitErr) ||
					submitErr.Result != zlink.SubmitInvalidArgument ||
					submitErr.InternalErrno() != int(syscall.EINVAL) {
					result.err = fmt.Errorf("concurrent submit error = %v, want EINVAL rejection", err)
					_ = first.Close()
					_ = second.Close()
					break
				}
				if string(first.Data()) != prefix+"-a" || string(second.Data()) != prefix+"-b" {
					result.err = fmt.Errorf("binding staging lost caller-owned parts after multipart rejection %q", prefix)
					_ = first.Close()
					_ = second.Close()
					break
				}
				_ = first.Close()
				_ = second.Close()
				result.rejected++
			}
			results <- result
		}(sender)
	}
	wg.Wait()
	close(results)
	successes := 0
	rejected := 0
	for result := range results {
		if result.err != nil {
			t.Fatal(result.err)
		}
		successes += result.successes
		rejected += result.rejected
	}
	if successes == 0 || rejected == 0 {
		t.Fatalf("concurrent multipart results: successes=%d rejected=%d; want both", successes, rejected)
	}

	seen := make(map[string]bool, successes)
	for i := 0; i < successes; i++ {
		received := &zlink.Received{}
		ok, err := f.dealerA.Recv(received, zlink.RecvFlagsNone)
		if err != nil || !ok {
			t.Fatalf("Recv() = (%v, %v)", ok, err)
		}
		parts := received.Parts()
		if len(parts) != 2 {
			_ = received.Close()
			t.Fatalf("multipart record has %d parts, want 2", len(parts))
		}
		first := string(parts[0].Data())
		if len(first) < 2 || first[len(first)-2:] != "-a" {
			_ = received.Close()
			t.Fatalf("first multipart part = %q", first)
		}
		prefix := first[:len(first)-2]
		if string(parts[1].Data()) != prefix+"-b" {
			_ = received.Close()
			t.Fatalf("multipart record %q was interleaved", prefix)
		}
		if seen[prefix] {
			_ = received.Close()
			t.Fatalf("multipart record %q was duplicated", prefix)
		}
		seen[prefix] = true
		_ = received.Close()
	}
	if len(seen) != successes {
		t.Fatalf("received %d records, want %d successful records", len(seen), successes)
	}
	t.Logf("concurrent multipart: attempts=%d successes=%d rejected=%d", senders*perSender, successes, rejected)
}

// The request submit is synchronous but keeps its completion channel: Core's
// reply callback completes it. The submit itself waits inside Core for the
// exact target's credit.
func TestRoutedRequestWaitsForCreditAndReplies(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	admitted := fillRoutedTargetUntilBackpressured(t, f.router, f.ridA)
	if err := f.router.SetSendTimeout(5 * time.Second); err != nil {
		t.Fatalf("router SetSendTimeout() error = %v", err)
	}

	submitted := make(chan (<-chan zlink.RequestReplyCompletion), 1)
	go func() {
		submitted <- f.router.Request(f.ridA).
			Bytes([]byte("request-after-credit")).
			Timeout(5 * time.Second).
			Submit(context.Background())
	}()

	drainRoutedAsync(t, f.dealerA, admitted)

	var request <-chan zlink.RequestReplyCompletion
	select {
	case request = <-submitted:
	case <-time.After(5 * time.Second):
		t.Fatal("request submit did not return after its credit came back")
	}

	replied := false
	for i := 0; i < 32 && !replied; i++ {
		received := recvRoutedAsync(t, f.dealerA)
		if received.HasRequestSeq() {
			parts := received.Parts()
			if len(parts) != 1 || string(parts[0].Data()) != "request-after-credit" {
				_ = received.Close()
				t.Fatalf("request payload was not preserved")
			}
			if err := received.Reply().Message(newMessage(t, "request-reply")).Submit(context.Background()); err != nil {
				_ = received.Close()
				t.Fatalf("request Reply() error = %v", err)
			}
			replied = true
		}
		_ = received.Close()
	}
	if !replied {
		t.Fatal("request never reached the exact target")
	}

	completion := awaitRequest(t, request)
	defer zlink.MultipartClose(completion.Parts)
	if completion.Err != nil {
		t.Fatalf("request completion error = %v", completion.Err)
	}
	if len(completion.Parts) != 1 || string(completion.Parts[0].Data()) != "request-reply" {
		t.Fatalf("request reply payload was not preserved")
	}
}

func TestRoutedRequestContextCompletesExactlyOnce(t *testing.T) {
	t.Run("cancel", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		ctx, cancel := context.WithCancel(context.Background())
		request := f.router.Request(f.ridA).
			Bytes([]byte("cancel-after-submit")).
			Timeout(10 * time.Second).
			Submit(ctx)
		cancel()
		completion := awaitRequest(t, request)
		if !errors.Is(completion.Err, context.Canceled) {
			t.Fatalf("cancel completion error = %v, want context.Canceled", completion.Err)
		}
		requireRequestCompletionClosed(t, request)
	})

	t.Run("deadline", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
		defer cancel()
		request := f.router.Request(f.ridA).
			Bytes([]byte("deadline-after-submit")).
			Timeout(10 * time.Second).
			Submit(ctx)
		completion := awaitRequest(t, request)
		if !errors.Is(completion.Err, context.DeadlineExceeded) {
			t.Fatalf("deadline completion error = %v, want context.DeadlineExceeded", completion.Err)
		}
		requireRequestCompletionClosed(t, request)
	})

	t.Run("core owned timeout", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		request := f.router.Request(f.ridA).
			Bytes([]byte("core-timeout")).
			Timeout(250 * time.Millisecond).
			Submit(context.Background())
		completion := awaitRequest(t, request)
		if completion.Result != zlink.RequestTimedOut {
			t.Fatalf("request completion result = %v, want RequestTimedOut", completion.Result)
		}
		requireRequestCompletionClosed(t, request)
	})
}

func TestRoutedBuilderCannotSubmitTwice(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	sendBuilder := f.router.SendTo(f.ridA).Bytes([]byte("submit-once"))
	if err := sendBuilder.Submit(context.Background()); err != nil {
		t.Fatalf("first send Submit() error = %v", err)
	}
	if err := sendBuilder.Submit(context.Background()); err == nil {
		t.Fatal("second send Submit() completed without a state error")
	}
	received := recvRoutedAsync(t, f.dealerA)
	_ = received.Close()

	requestBuilder := f.router.Request(f.ridA).
		Bytes([]byte("request-submit-once")).
		Timeout(2 * time.Second)
	first := requestBuilder.Submit(context.Background())
	second := awaitRequest(t, requestBuilder.Submit(context.Background()))
	if second.Err == nil {
		t.Fatal("second request Submit() completed without a state error")
	}
	request := recvRoutedAsync(t, f.dealerA)
	if err := request.Reply().Message(newMessage(t, "submit-once-reply")).Submit(context.Background()); err != nil {
		_ = request.Close()
		t.Fatalf("request Reply() error = %v", err)
	}
	_ = request.Close()
	completion := awaitRequest(t, first)
	defer zlink.MultipartClose(completion.Parts)
	if completion.Err != nil {
		t.Fatalf("first request Submit() error = %v", completion.Err)
	}
}
