package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

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
		ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
		err := awaitRoutedSend(t, router.SendTo(rid).Bytes([]byte("route-ready")).Submit(ctx))
		cancel()
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

func fillRoutedTargetUntilWaiting(t testing.TB, router *zlink.RouterSocket, rid zlink.RoutingID, ctx context.Context) <-chan error {
	t.Helper()
	payload := make([]byte, routedAsyncTestHWM)
	for attempt := 0; attempt < 16; attempt++ {
		completion := router.SendTo(rid).Bytes(payload).Submit(ctx)
		timer := time.NewTimer(100 * time.Millisecond)
		select {
		case err, ok := <-completion:
			timer.Stop()
			if !ok {
				t.Fatal("routed send completion closed without a result")
			}
			if err != nil {
				t.Fatalf("fill send error = %v", err)
			}
		case <-timer.C:
			select {
			case err, ok := <-completion:
				if !ok {
					t.Fatal("routed send completion closed without a result")
				}
				if err != nil {
					t.Fatalf("fill send error = %v", err)
				}
			default:
				return completion
			}
		}
	}
	t.Fatal("target did not reach HWM")
	return nil
}

func awaitRoutedSendWithin(t testing.TB, completion <-chan error, timeout time.Duration) error {
	t.Helper()
	select {
	case err, ok := <-completion:
		if !ok {
			t.Fatal("routed send completion closed without a result")
		}
		return err
	case <-time.After(timeout):
		t.Fatal("timed out waiting for routed send completion")
		return nil
	}
}

func requireCompletionClosed(t testing.TB, completion <-chan error) {
	t.Helper()
	select {
	case _, ok := <-completion:
		if ok {
			t.Fatal("routed send completion produced more than one terminal result")
		}
	case <-time.After(time.Second):
		t.Fatal("routed send completion did not close after its terminal result")
	}
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

func drainUntilRoutedSendCompletes(t testing.TB, dealer *zlink.DealerSocket, completion <-chan error) {
	t.Helper()
	for drained := 0; drained < 16; drained++ {
		select {
		case err, ok := <-completion:
			if !ok {
				t.Fatal("routed send completion closed without a result")
			}
			if err != nil {
				t.Fatalf("routed send after exact wake error = %v", err)
			}
			return
		default:
		}
		received := recvRoutedAsync(t, dealer)
		_ = received.Close()
		select {
		case err, ok := <-completion:
			if !ok {
				t.Fatal("routed send completion closed without a result")
			}
			if err != nil {
				t.Fatalf("routed send after exact wake error = %v", err)
			}
			return
		case <-time.After(250 * time.Millisecond):
		}
	}
	t.Fatal("exact target did not wake after its credit was returned")
}

func TestRoutedAsyncBlockedTargetDoesNotBlockOtherAndExactWake(t *testing.T) {
	f := newRoutedAsyncFixture(t, true)
	defer f.close()

	blockedA := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
	if err := awaitRoutedSendWithin(t, f.router.SendTo(f.ridB).Bytes([]byte("b-progress")).Submit(context.Background()), 2*time.Second); err != nil {
		t.Fatalf("target B send error = %v", err)
	}
	receivedB := recvRoutedAsync(t, f.dealerB)
	if len(receivedB.Parts()) != 1 || string(receivedB.Parts()[0].Data()) != "b-progress" {
		t.Fatalf("target B payload was not preserved")
	}
	_ = receivedB.Close()

	select {
	case err := <-blockedA:
		t.Fatalf("target A completed before A returned credit: %v", err)
	default:
	}
	drainUntilRoutedSendCompletes(t, f.dealerA, blockedA)
	if err := f.router.Close(); err != nil {
		t.Fatalf("router Close() after exact wake error = %v", err)
	}
	f.router = nil
}

func TestRoutedAsyncZeroSendTimeoutDoesNotWaitBehindTargetQueue(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	_ = fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
	if err := f.router.SetSendTimeout(0); err != nil {
		t.Fatalf("router SetSendTimeout(0) error = %v", err)
	}
	started := time.Now()
	err := awaitRoutedSendWithin(
		t,
		f.router.SendTo(f.ridA).Bytes([]byte("zero-timeout")).Submit(context.Background()),
		time.Second,
	)
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

func TestRoutedAsyncDetachCompletesExactlyOnce(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	blocked := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
	if err := f.dealerA.Close(); err != nil {
		t.Fatalf("dealer A Close() error = %v", err)
	}
	f.dealerA = nil
	if err := awaitRoutedSendWithin(t, blocked, 2*time.Second); err == nil {
		t.Fatal("detached target completed without an error")
	}
	requireCompletionClosed(t, blocked)
}

func TestRoutedAsyncContextAndCloseCompleteExactlyOnce(t *testing.T) {
	t.Run("cancel", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		ctx, cancel := context.WithCancel(context.Background())
		blocked := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, ctx)
		cancel()
		if err := awaitRoutedSendWithin(t, blocked, 2*time.Second); !errors.Is(err, context.Canceled) {
			t.Fatalf("cancel completion error = %v, want context.Canceled", err)
		}
		requireCompletionClosed(t, blocked)
	})

	t.Run("deadline", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
		defer cancel()
		blocked := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, ctx)
		if err := awaitRoutedSendWithin(t, blocked, 2*time.Second); !errors.Is(err, context.DeadlineExceeded) {
			t.Fatalf("deadline completion error = %v, want context.DeadlineExceeded", err)
		}
		requireCompletionClosed(t, blocked)
	})

	t.Run("socket close", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		blocked := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
		if err := f.router.Close(); err != nil {
			t.Fatalf("router Close() error = %v", err)
		}
		f.router = nil
		if err := awaitRoutedSendWithin(t, blocked, 2*time.Second); err == nil {
			t.Fatal("socket-close completion did not contain an error")
		}
		requireCompletionClosed(t, blocked)
	})
}

func TestRoutedAsyncCancelBeforeQueuedAttemptPreventsWireSubmit(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	_ = fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
	ctx, cancel := context.WithCancel(context.Background())
	canceled := f.router.SendTo(f.ridA).Bytes([]byte("must-not-reach-wire")).Submit(ctx)
	cancel()
	completionErr := awaitRoutedSendWithin(t, canceled, 2*time.Second)
	if !errors.Is(completionErr, context.Canceled) {
		t.Fatalf("queued cancel completion error = %v, want context.Canceled", completionErr)
	}
	requireCompletionClosed(t, canceled)

	for drained := 0; drained < 16; drained++ {
		var received zlink.Received
		ok, err := f.dealerA.Recv(&received, zlink.RecvFlagsDontWait)
		if err != nil {
			t.Fatalf("drain Recv() error = %v", err)
		}
		if !ok {
			return
		}
		parts := received.Parts()
		if len(parts) == 1 && string(parts[0].Data()) == "must-not-reach-wire" {
			_ = received.Close()
			t.Fatal("context-canceled queued send reached the wire")
		}
		_ = received.Close()
	}
	var received zlink.Received
	if ok, err := f.dealerA.Recv(&received, zlink.RecvFlagsDontWait); err != nil {
		t.Fatalf("final drain Recv() error = %v", err)
	} else if ok {
		_ = received.Close()
		t.Fatal("test did not finish draining the pre-cancel queue")
	}
}

func TestRoutedAsyncMultipartRecordsDoNotInterleave(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()
	if err := f.router.SetSendHighWaterMark(1 << 20); err != nil {
		t.Fatalf("router SetSendHighWaterMark() error = %v", err)
	}
	if err := f.dealerA.SetReceiveHighWaterMark(1 << 20); err != nil {
		t.Fatalf("dealer A SetReceiveHighWaterMark() error = %v", err)
	}

	const records = 24
	completions := make([]<-chan error, records)
	for i := 0; i < records; i++ {
		prefix := fmt.Sprintf("record-%02d", i)
		completions[i] = f.router.SendTo(f.ridA).
			Bytes([]byte(prefix + "-a")).
			Bytes([]byte(prefix + "-b")).
			Bytes([]byte(prefix + "-c")).
			Submit(context.Background())
	}
	for i, completion := range completions {
		if err := awaitRoutedSendWithin(t, completion, 2*time.Second); err != nil {
			t.Fatalf("multipart send %d error = %v", i, err)
		}
	}

	seen := make(map[string]bool, records)
	for i := 0; i < records; i++ {
		received := recvRoutedAsync(t, f.dealerA)
		parts := received.Parts()
		if len(parts) != 3 {
			_ = received.Close()
			t.Fatalf("multipart record has %d parts, want 3", len(parts))
		}
		first := string(parts[0].Data())
		if len(first) < 2 || first[len(first)-2:] != "-a" {
			_ = received.Close()
			t.Fatalf("first multipart part = %q", first)
		}
		prefix := first[:len(first)-2]
		if string(parts[1].Data()) != prefix+"-b" || string(parts[2].Data()) != prefix+"-c" {
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
}

func TestRoutedAsyncRequestWaitsForCreditAndReplies(t *testing.T) {
	f := newRoutedAsyncFixture(t, true)
	defer f.close()

	blockedSend := fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
	request := f.router.Request(f.ridA).
		Bytes([]byte("request-after-credit")).
		Timeout(4 * time.Second).
		Submit(context.Background())
	if err := awaitRoutedSendWithin(t, f.router.SendTo(f.ridB).Bytes([]byte("b-during-request-wait")).Submit(context.Background()), 2*time.Second); err != nil {
		t.Fatalf("target B send error = %v", err)
	}
	receivedB := recvRoutedAsync(t, f.dealerB)
	_ = receivedB.Close()

	blockedDone := false
	replied := false
	for receivedCount := 0; receivedCount < 20 && !replied; receivedCount++ {
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
		if !blockedDone {
			select {
			case err, ok := <-blockedSend:
				if !ok || err != nil {
					t.Fatalf("blocked send completion = (%v, %v)", err, ok)
				}
				blockedDone = true
			default:
			}
		}
	}
	if !replied {
		t.Fatal("request was not admitted after exact-target credit returned")
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

func TestRoutedAsyncRequestContextAndCloseCompleteExactlyOnce(t *testing.T) {
	t.Run("cancel", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		_ = fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
		ctx, cancel := context.WithCancel(context.Background())
		request := f.router.Request(f.ridA).
			Bytes([]byte("cancel-while-waiting")).
			Timeout(4 * time.Second).
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
		_ = fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
		ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
		defer cancel()
		request := f.router.Request(f.ridA).
			Bytes([]byte("deadline-while-waiting")).
			Timeout(4 * time.Second).
			Submit(ctx)
		completion := awaitRequest(t, request)
		if !errors.Is(completion.Err, context.DeadlineExceeded) {
			t.Fatalf("deadline completion error = %v, want context.DeadlineExceeded", completion.Err)
		}
		requireRequestCompletionClosed(t, request)
	})

	t.Run("socket close", func(t *testing.T) {
		f := newRoutedAsyncFixture(t, false)
		defer f.close()
		_ = fillRoutedTargetUntilWaiting(t, f.router, f.ridA, context.Background())
		request := f.router.Request(f.ridA).
			Bytes([]byte("close-while-waiting")).
			Timeout(4 * time.Second).
			Submit(context.Background())
		if err := f.router.Close(); err != nil {
			t.Fatalf("router Close() error = %v", err)
		}
		f.router = nil
		completion := awaitRequest(t, request)
		if completion.Err == nil {
			t.Fatal("socket-close request completion did not contain an error")
		}
		requireRequestCompletionClosed(t, request)
	})
}

func TestRoutedAsyncBuilderCannotSubmitTwice(t *testing.T) {
	f := newRoutedAsyncFixture(t, false)
	defer f.close()

	sendBuilder := f.router.SendTo(f.ridA).Bytes([]byte("submit-once"))
	if err := awaitRoutedSendWithin(t, sendBuilder.Submit(context.Background()), 2*time.Second); err != nil {
		t.Fatalf("first send Submit() error = %v", err)
	}
	if err := awaitRoutedSendWithin(t, sendBuilder.Submit(context.Background()), 2*time.Second); err == nil {
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
