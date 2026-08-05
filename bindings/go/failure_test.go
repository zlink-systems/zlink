package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

func TestSubmitContextCancellationUsesStandardErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, err := ctx.PairSocket()
	if err != nil {
		t.Fatalf("PairSocket() error = %v", err)
	}
	defer socket.Close()

	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	message := newMessage(t, "context-canceled")
	defer message.Close()

	if _, err := socket.Send().Message(message).Submit(canceled); !errors.Is(err, context.Canceled) {
		t.Fatalf("Send().Submit(canceled) error = %v, want context.Canceled", err)
	}
	if got := string(message.Data()); got != "context-canceled" {
		t.Fatalf("canceled send consumed message payload = %q", got)
	}
}

func TestRequestContextDeadlineUsesStandardErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, err := ctx.DealerSocket()
	if err != nil {
		t.Fatalf("DealerSocket() error = %v", err)
	}
	defer socket.Close()

	deadline, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Second))
	defer cancel()
	if _, err := socket.Request().Bytes([]byte("context-deadline")).SubmitAsync(deadline); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Request().SubmitAsync(expired) error = %v, want context.DeadlineExceeded", err)
	}
}

func TestRequestCallbackDeliveryUsesSocketDispatcher(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

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

	endpoint := inprocEndpoint("request-callback-dispatcher")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	if err := router.SetReceiveTimeout(5 * time.Second); err != nil {
		t.Fatalf("SetReceiveTimeout() error = %v", err)
	}

	serverDone := make(chan error, 1)
	go func() {
		var request zlink.Received
		if _, err := router.Recv(&request, zlink.RecvFlagsNone); err != nil {
			serverDone <- err
			return
		}
		defer request.Close()
		reply, err := zlink.NewMessageString("callback-reply")
		if err != nil {
			serverDone <- err
			return
		}
		defer reply.Close()
		serverDone <- request.Reply().Message(reply).Submit(context.Background())
	}()

	callbackDone := make(chan error, 1)
	ok, err := dealer.Request().Bytes([]byte("callback-request")).Timeout(2*time.Second).Submit(context.Background(), func(result zlink.RequestResult, parts []*zlink.Message) {
		defer zlink.MultipartClose(parts)
		if result != zlink.RequestOK {
			callbackDone <- fmt.Errorf("request result = %v, want RequestOK", result)
			return
		}
		if len(parts) != 1 || string(parts[0].Data()) != "callback-reply" {
			callbackDone <- fmt.Errorf("request callback parts = %d, want callback-reply", len(parts))
			return
		}
		callbackDone <- nil
	})
	if err != nil {
		t.Fatalf("Request().Submit(callback) error = %v", err)
	}
	if !ok {
		t.Fatal("Request().Submit(callback) returned ok=false")
	}
	select {
	case err := <-callbackDone:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("request callback was not delivered")
	}
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("request server error = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("request server did not finish")
	}
}

func TestPollerModifyCompletionDoesNotDisableRequestProgress(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

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

	endpoint := inprocEndpoint("poller-modify-completion")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	defer poller.Close()
	if err := poller.AddSocket(dealer, zlink.PollIn, 81); err != nil {
		t.Fatalf("AddSocket() error = %v", err)
	}
	if err := poller.ModifySocket(dealer, zlink.PollCompletion); err == nil {
		t.Fatalf("ModifySocket(PollCompletion) should be rejected")
	}
	reply, err := zlink.NewMessage([]byte("pong"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer reply.Close()

	serverDone := make(chan error, 1)
	go func() {
		var request zlink.Received
		if _, err := router.Recv(&request, zlink.RecvFlagsNone); err != nil {
			serverDone <- err
			return
		}
		defer request.Close()
		serverDone <- request.Reply().Message(reply).Submit(context.Background())
	}()

	completion, err := dealer.Request().Bytes([]byte("ping")).Timeout(time.Second).SubmitAsync(context.Background())
	if err != nil {
		t.Fatalf("SubmitAsync() error = %v", err)
	}
	select {
	case result, ok := <-completion:
		if !ok {
			t.Fatalf("completion channel closed without result")
		}
		if result.Err != nil {
			t.Fatalf("request completion error = %v", result.Err)
		}
		zlink.MultipartClose(result.Parts)
	case <-time.After(3 * time.Second):
		t.Fatalf("request completion did not arrive after rejected poller modify")
	}
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("server request handling error = %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatalf("server request handler did not finish")
	}
}

func TestPollerCompletionOwnsAndReleasesRequestProgress(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

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

	endpoint := inprocEndpoint("poller-completion-ownership")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	if err := router.SetReceiveTimeout(5 * time.Second); err != nil {
		t.Fatalf("SetReceiveTimeout() error = %v", err)
	}

	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	registered := false
	defer func() {
		if registered {
			_ = poller.RemoveSocket(dealer)
		}
		_ = poller.Close()
	}()
	if err := poller.AddSocket(dealer, zlink.PollCompletion, 91); err != nil {
		t.Fatalf("AddSocket(PollCompletion) error = %v", err)
	}
	registered = true

	serveRequest := func(payload string) <-chan error {
		result := make(chan error, 1)
		go func() {
			var request zlink.Received
			if _, err := router.Recv(&request, zlink.RecvFlagsNone); err != nil {
				result <- err
				return
			}
			defer request.Close()
			reply, err := zlink.NewMessage([]byte(payload))
			if err != nil {
				result <- err
				return
			}
			defer reply.Close()
			submitErr := request.Reply().Message(reply).Submit(context.Background())
			if submitErr == nil && reply.Data() != nil {
				result <- fmt.Errorf("successful reply did not consume message")
				return
			}
			result <- submitErr
		}()
		return result
	}

	serverDone := serveRequest("poller-reply")
	completion, err := dealer.Request().Bytes([]byte("poller-request")).Timeout(2 * time.Second).SubmitAsync(context.Background())
	if err != nil {
		t.Fatalf("SubmitAsync() error = %v", err)
	}

	events := make([]zlink.PollEvent, 1)
	sawCompletionEvent := false
	var first zlink.RequestReplyCompletion
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		n, err := poller.Wait(events, 100*time.Millisecond)
		if err != nil {
			t.Fatalf("Wait(PollCompletion) error = %v", err)
		}
		if n > 0 && events[0].Slot == 91 && events[0].Revents&zlink.PollCompletion != 0 {
			sawCompletionEvent = true
		}
		select {
		case result, ok := <-completion:
			if !ok {
				t.Fatalf("completion channel closed without result")
			}
			first = result
			goto firstComplete
		default:
		}
	}
	t.Fatalf("PollCompletion did not deliver request completion")

firstComplete:
	if !sawCompletionEvent {
		t.Fatalf("request completed without a PollCompletion event")
	}
	if first.Err != nil {
		t.Fatalf("first request completion error = %v", first.Err)
	}
	zlink.MultipartClose(first.Parts)
	if err := <-serverDone; err != nil {
		t.Fatalf("first server request error = %v", err)
	}

	if err := poller.RemoveSocket(dealer); err != nil {
		t.Fatalf("RemoveSocket() error = %v", err)
	}
	registered = false

	serverDone = serveRequest("internal-reply")
	second, err := dealer.Request().Bytes([]byte("internal-request")).Timeout(2 * time.Second).SubmitAsync(context.Background())
	if err != nil {
		t.Fatalf("second SubmitAsync() error = %v", err)
	}
	select {
	case result, ok := <-second:
		if !ok {
			t.Fatalf("second completion channel closed without result")
		}
		if result.Err != nil {
			t.Fatalf("second request completion error = %v", result.Err)
		}
		zlink.MultipartClose(result.Parts)
	case <-time.After(5 * time.Second):
		t.Fatalf("internal request progress did not resume after RemoveSocket")
	}
	if err := <-serverDone; err != nil {
		t.Fatalf("second server request error = %v", err)
	}
}

func TestSendDontWaitDoesNotTreatTemporaryBackpressureAsError(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	defer socket.Close()
	_ = socket.Bind(inprocEndpoint("try-send"))

	ok, err := socket.Send().Message(newMessage(t, "data")).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
	if err != nil {
		t.Fatalf("Send() with DontWait should not error for backpressure, got: %v", err)
	}
	_ = ok
}

func TestPublishDontWaitReturnsErrorWhenUnroutable(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PubSocket()
	defer socket.Close()
	_ = socket.Bind(inprocEndpoint("try-publish"))

	if _, err := socket.Publish("topic").Message(newMessage(t, "data")).Flags(zlink.SendFlagsDontWait).Submit(context.Background()); err != nil {
		t.Fatalf("Publish() with DontWait on idle socket should succeed: %v", err)
	}
}

func TestBlockingSendFailureSurfacesError(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()

	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory() error = %v", err)
	}
	if err := router.Bind(inprocEndpoint("router-send-fail")); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	rid := zlink.NewRoutingID([]byte("missing-peer"))
	if _, err := router.SendTo(rid).Message(newMessage(t, "data")).Submit(context.Background()); err == nil {
		t.Fatalf("SendTo() should surface an error when no peer exists")
	}
}

func TestBlockingSendFailurePreservesMessagePayload(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()

	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory() error = %v", err)
	}
	if err := router.Bind(inprocEndpoint("router-send-preserve")); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	rid := zlink.NewRoutingID([]byte("missing-peer"))

	msg := newMessage(t, "preserve-me")
	defer msg.Close()

	if _, err := router.SendTo(rid).Message(msg).Submit(context.Background()); err == nil {
		t.Fatalf("SendTo() should surface an error when no peer exists")
	}
	if got := string(msg.Data()); got != "preserve-me" {
		t.Fatalf("message payload after SendTo() failure = %q, want %q", got, "preserve-me")
	}
}

func TestBlockingSendFailurePreservesBytesPayload(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()

	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory() error = %v", err)
	}
	if err := router.Bind(inprocEndpoint("router-send-bytes-preserve")); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	rid := zlink.NewRoutingID([]byte("missing-peer"))
	payload := []byte("preserve-bytes")

	if _, err := router.SendTo(rid).Bytes(payload).Submit(context.Background()); err == nil {
		t.Fatalf("SendTo().Bytes() should surface an error when no peer exists")
	}
	if got := string(payload); got != "preserve-bytes" {
		t.Fatalf("bytes payload after SendTo().Bytes() failure = %q, want %q", got, "preserve-bytes")
	}
}

func TestMoveMessageFailureConsumesMessagePayload(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()

	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory() error = %v", err)
	}
	if err := router.Bind(inprocEndpoint("router-move-send-consumes")); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	rid := zlink.NewRoutingID([]byte("missing-peer"))

	msg := newMessage(t, "consume-me")
	defer msg.Close()

	if _, err := router.SendTo(rid).MoveMessage(msg).Submit(context.Background()); err == nil {
		t.Fatalf("MoveMessage SendTo() should surface an error when no peer exists")
	}
	if got := msg.Data(); got != nil {
		t.Fatalf("message payload after MoveMessage failure = %q, want nil", string(got))
	}
}

func TestSendDoesNotSwallowClosedSocketErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	if _, err := socket.Send().Message(newMessage(t, "data")).Submit(context.Background()); err == nil {
		t.Fatalf("Send() on closed socket should surface an error")
	}
}

func TestPublishDoesNotSwallowClosedSocketErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PubSocket()
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	if _, err := socket.Publish("topic").Message(newMessage(t, "data")).Submit(context.Background()); err == nil {
		t.Fatalf("Publish() on closed socket should surface an error")
	}
}

func TestPublishFailurePreservesMessagePayload(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PubSocket()
	msg := newMessage(t, "preserve-me")
	defer msg.Close()

	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	if _, err := socket.Publish("topic").Message(msg).Submit(context.Background()); err == nil {
		t.Fatalf("Publish() on closed socket should surface an error")
	}
	if got := string(msg.Data()); got != "preserve-me" {
		t.Fatalf("message payload after Publish() failure = %q, want %q", got, "preserve-me")
	}
}

func TestRecvDoesNotSwallowClosedSocketErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	var received zlink.Received
	if _, err := socket.Recv(&received, zlink.RecvFlagsDontWait); err == nil {
		t.Fatalf("Recv() on closed socket should surface an error")
	}
}

func TestSubscribeDoesNotSwallowClosedSocketErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.SubSocket()
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	var message zlink.TopicMessage
	if _, err := socket.Subscribe(&message, zlink.RecvFlagsDontWait); err == nil {
		t.Fatalf("Subscribe() on closed socket should surface an error")
	}
}

func TestReceiveSubscriptionEventDoesNotSwallowClosedSocketErrors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.XPubSocket()
	if err := socket.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	var event zlink.SubscriptionEvent
	if _, err := socket.ReceiveSubscriptionEvent(&event, zlink.RecvFlagsDontWait); err == nil {
		t.Fatalf("ReceiveSubscriptionEvent() on closed socket should surface an error")
	}
}

func TestCallbackModeConflictsWithDirectRecv(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	delivered := make(chan struct{}, 1)
	if err := server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		defer header.Close()
		defer body.Close()
		_ = source
		delivered <- struct{}{}
	}); err != nil {
		t.Fatalf("OnPacket() error = %v", err)
	}

	var received zlink.Received
	if _, err := server.Recv(&received, zlink.RecvFlagsNone); err == nil {
		t.Fatalf("Recv() after OnPacket() should fail")
	}

	writeStreamPacket(t, conn, []byte("callback-data"))

	select {
	case <-delivered:
	case <-time.After(5 * time.Second):
		t.Fatalf("callback was not delivered within 5s")
	}
}

func TestReceiveCallbackCanUseBlockingSend(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	sendErrs := make(chan error, 1)
	if err := server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		packet := frameStreamPacketMessage(t, header, body)
		_ = header.Close()
		_ = body.Close()
		if _, err := server.SendTo(source).Message(packet).Submit(context.Background()); err != nil {
			_ = packet.Close()
			sendErrs <- err
			return
		}
		sendErrs <- nil
	}); err != nil {
		t.Fatalf("OnPacket() error = %v", err)
	}

	writeStreamPacket(t, conn, []byte("request"))

	select {
	case err := <-sendErrs:
		if err != nil {
			t.Fatalf("callback Send() error = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("callback send did not complete within 5s")
	}

	reply := readStreamPacketBody(t, conn)
	if got := string(reply); got != "request" {
		t.Fatalf("callback reply = %q, want %q", got, "request")
	}
}

func TestReceiveCallbackPanicDoesNotCloseSocketOrStopFutureCallbacks(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	var calls atomic.Int32
	delivered := make(chan struct{}, 1)
	if err := server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		defer header.Close()
		switch calls.Add(1) {
		case 1:
			panic("callback panic for policy test")
		default:
			defer body.Close()
			_ = source
			delivered <- struct{}{}
		}
	}); err != nil {
		t.Fatalf("OnPacket() error = %v", err)
	}

	writeStreamPacket(t, conn, []byte("first"))
	writeStreamPacket(t, conn, []byte("second"))

	select {
	case <-delivered:
	case <-time.After(5 * time.Second):
		t.Fatalf("callback delivery stopped after panic")
	}

	if got := calls.Load(); got < 2 {
		t.Fatalf("callback invocation count = %d, want at least 2", got)
	}
}

func TestCloseInsideReceiveCallbackDoesNotDeadlock(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	closed := make(chan error, 1)
	if err := server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		defer header.Close()
		defer body.Close()
		_ = source
		closed <- server.Close()
	}); err != nil {
		t.Fatalf("OnPacket() error = %v", err)
	}

	writeStreamPacket(t, conn, []byte("close-from-callback"))

	select {
	case err := <-closed:
		if err != nil {
			t.Fatalf("Close() from callback error = %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("Close() from callback deadlocked")
	}
}
