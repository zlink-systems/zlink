package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"
	"syscall"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
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

	if err := socket.Send().Message(message).Submit(canceled); !errors.Is(err, context.Canceled) {
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
	parts, err := socket.Request().Bytes([]byte("context-deadline")).Submit(deadline)
	if parts != nil || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("Request().Submit(expired) = (%v, %v), want (nil, context.DeadlineExceeded)", parts, err)
	}
}

func TestRequestSubmitReturnsReply(t *testing.T) {
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

	endpoint := inprocEndpoint("request-submit-reply")
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
		reply, err := zlink.NewMessageString("channel-reply")
		if err != nil {
			serverDone <- err
			return
		}
		defer reply.Close()
		serverDone <- request.Reply().Message(reply).Submit(context.Background())
	}()

	parts, err := dealer.Request().Bytes([]byte("channel-request")).Timeout(2 * time.Second).Submit(context.Background())
	defer zlink.MultipartClose(parts)
	if err != nil {
		t.Fatalf("request error = %v", err)
	}
	if len(parts) != 1 || string(parts[0].Data()) != "channel-reply" {
		t.Fatalf("request parts = %d, want channel-reply", len(parts))
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

func TestPollerModifyCompletionDoesNotDisableRequestCompletion(t *testing.T) {
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
	if err := poller.ModifySocket(dealer, zlink.PollCompletion); err != nil {
		t.Fatalf("ModifySocket(PollCompletion) error = %v", err)
	}
	if err := poller.ModifySocket(dealer, zlink.PollIn); err != nil {
		t.Fatalf("ModifySocket(PollIn) error = %v", err)
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

	parts, err := dealer.Request().Bytes([]byte("ping")).Timeout(time.Second).Submit(context.Background())
	if err != nil {
		t.Fatalf("request error after owner transfer = %v", err)
	}
	zlink.MultipartClose(parts)
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("server request handling error = %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatalf("server request handler did not finish")
	}
}

func TestPollerCompletionOwnsRequestProgressAndTransfersBack(t *testing.T) {
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
	type requestResult struct {
		parts []*zlink.Message
		err   error
	}
	completion := make(chan requestResult, 1)
	go func() {
		parts, err := dealer.Request().Bytes([]byte("poller-request")).Timeout(2 * time.Second).Submit(context.Background())
		completion <- requestResult{parts: parts, err: err}
	}()

	events := make([]zlink.PollEvent, 1)
	sawCompletionEvent := false
	var first requestResult
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
		case result := <-completion:
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
	if first.err != nil {
		t.Fatalf("first request completion error = %v", first.err)
	}
	zlink.MultipartClose(first.parts)
	if err := <-serverDone; err != nil {
		t.Fatalf("first server request error = %v", err)
	}

	if err := poller.RemoveSocket(dealer); err != nil {
		t.Fatalf("RemoveSocket() error = %v", err)
	}
	registered = false

	serverDone = serveRequest("internal-reply")
	second, err := dealer.Request().Bytes([]byte("internal-request")).Timeout(2 * time.Second).Submit(context.Background())
	if err != nil {
		t.Fatalf("request completion did not continue after RemoveSocket: %v", err)
	}
	zlink.MultipartClose(second)
	if err := <-serverDone; err != nil {
		t.Fatalf("second server request error = %v", err)
	}
}

func TestSendDoesNotExposeFlags(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	defer socket.Close()
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	err := socket.Send().Message(newMessage(t, "data")).Submit(canceled)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Send() cancellation error = %v", err)
	}
}

func TestLossyPublishDontWaitSucceedsWithoutPeer(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PubSocket()
	defer socket.Close()
	_ = socket.Bind(inprocEndpoint("try-publish"))

	if _, err := socket.Publish("topic").Message(newMessage(t, "data")).Flags(zlink.SendFlagsDontWait).Submit(context.Background()); err != nil {
		t.Fatalf("Publish() with DontWait on idle socket should succeed: %v", err)
	}
}

func TestNoDropPublishDontWaitReportsBackpressureAsState(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	publisher, _ := ctx.XPubSocket()
	subscriber, _ := ctx.SubSocket()
	defer publisher.Close()
	defer subscriber.Close()
	const recordHWM = 65_536 + 1_024
	if err := publisher.SetNoDrop(true); err != nil {
		t.Fatalf("SetNoDrop(true) error = %v", err)
	}
	if err := publisher.SetSendHighWaterMark(recordHWM); err != nil {
		t.Fatalf("publisher SetSendHighWaterMark() error = %v", err)
	}
	if err := subscriber.SetReceiveHighWaterMark(recordHWM); err != nil {
		t.Fatalf("subscriber SetReceiveHighWaterMark() error = %v", err)
	}
	if err := publisher.SetReceiveTimeout(5 * time.Second); err != nil {
		t.Fatalf("publisher SetReceiveTimeout() error = %v", err)
	}

	const topic = "dontwait-full"
	endpoint := inprocEndpoint("xpub-dontwait-backpressure")
	if err := publisher.Bind(endpoint); err != nil {
		t.Fatalf("publisher Bind() error = %v", err)
	}
	if err := subscriber.SetSubscription(topic); err != nil {
		t.Fatalf("subscriber SetSubscription() error = %v", err)
	}
	if err := subscriber.Connect(endpoint); err != nil {
		t.Fatalf("subscriber Connect() error = %v", err)
	}
	var subscription zlink.SubscriptionEvent
	if ok, err := publisher.ReceiveSubscriptionEvent(&subscription, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("ReceiveSubscriptionEvent() = (%v, %v), want (true, nil)", ok, err)
	}
	if !subscription.Subscribed() || subscription.Topic() != topic {
		t.Fatalf("subscription = (%v, %q), want (true, %q)", subscription.Subscribed(), subscription.Topic(), topic)
	}

	payload := make([]byte, 65_536)
	for attempt := 0; attempt < 64; attempt++ {
		accepted, err := publisher.Publish(topic).Bytes(payload).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
		if err != nil {
			t.Fatalf("DONTWAIT publish attempt %d error = %v", attempt, err)
		}
		if !accepted {
			return
		}
	}
	t.Fatal("NODROP DONTWAIT publish did not report backpressure")
}

func TestTargetedSendFailureSurfacesError(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()
	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory(true) error = %v", err)
	}
	if err := router.Bind(inprocEndpoint("router-send-fail")); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	rid := zlink.NewRoutingID([]byte("missing-peer"))
	message := newMessage(t, "data")
	defer message.Close()
	err := router.SendTo(rid).Message(message).Submit(context.Background())
	var submitErr *zlink.SubmitError
	if !errors.As(err, &submitErr) || submitErr.Result != zlink.SubmitNotConnected {
		t.Fatalf("SendTo(missing RID) error = %v, want SubmitNotConnected", err)
	}
	if !errors.Is(err, syscall.EHOSTUNREACH) {
		t.Fatalf("SendTo(missing RID) error = %v, want EHOSTUNREACH", err)
	}
}

func TestTargetedSendFailurePreservesMessagePayload(t *testing.T) {
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

	if err := router.SendTo(rid).Message(msg).Submit(context.Background()); err == nil {
		t.Fatalf("SendTo() should surface an error when no peer exists")
	}
	if got := string(msg.Data()); got != "preserve-me" {
		t.Fatalf("message payload after SendTo() failure = %q, want %q", got, "preserve-me")
	}
}

func TestTargetedSendFailurePreservesBytesPayload(t *testing.T) {
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

	if err := router.SendTo(rid).Bytes(payload).Submit(context.Background()); err == nil {
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

	if err := router.SendTo(rid).MoveMessage(msg).Submit(context.Background()); err == nil {
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

	if err := socket.Send().Message(newMessage(t, "data")).Submit(context.Background()); err == nil {
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

func TestStreamPacketNoDataLeavesOutputEmpty(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	server, _ := ctx.StreamSocket()
	defer server.Close()
	if err := server.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}
	var packet zlink.StreamPacket
	if ok, err := server.RecvPacket(&packet, zlink.RecvFlagsDontWait); err != nil || ok {
		t.Fatalf("RecvPacket(DontWait) = (%v, %v), want (false, nil)", ok, err)
	}
	if !packet.Empty() || packet.HasRoutingID() || packet.Header() != nil || packet.Body() != nil {
		t.Fatalf("no-data packet must remain empty")
	}
}

func TestStreamPacketPullCanUseManagedSend(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()
	if err := server.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	writeStreamPacket(t, conn, []byte("request"))
	var received zlink.StreamPacket
	if ok, err := server.RecvPacket(&received, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("RecvPacket() = (%v, %v), want (true, nil)", ok, err)
	}
	packet := frameStreamPacketMessage(t, received.Header(), received.Body())
	if err := server.SendTo(received.RoutingID()).Message(packet).Submit(context.Background()); err != nil {
		t.Fatalf("packet Send() error = %v", err)
	}

	reply := readStreamPacketBody(t, conn)
	if got := string(reply); got != "request" {
		t.Fatalf("packet reply = %q, want %q", got, "request")
	}
}

func TestStreamPacketOutputResetsAndReuses(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()
	if err := server.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	writeStreamPacket(t, conn, []byte("first"))
	var packet zlink.StreamPacket
	if ok, err := server.RecvPacket(&packet, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("first RecvPacket() = (%v, %v)", ok, err)
	}
	firstBody := packet.Body()
	writeStreamPacket(t, conn, []byte("second"))
	if ok, err := server.RecvPacket(&packet, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("second RecvPacket() = (%v, %v)", ok, err)
	}
	if packet.Body() == firstBody || string(packet.Body().Data()) != "second" {
		t.Fatalf("reused packet did not replace its body")
	}
}

func TestStreamPacketCloseRestoresEmptyAccessors(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.StreamSocket()
	defer server.Close()
	if err := server.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	writeStreamPacket(t, conn, []byte("close-packet"))
	var packet zlink.StreamPacket
	if ok, err := server.RecvPacket(&packet, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("RecvPacket() = (%v, %v)", ok, err)
	}
	if err := packet.Close(); err != nil {
		t.Fatalf("StreamPacket.Close() error = %v", err)
	}
	if !packet.Empty() || packet.HasRoutingID() || packet.Header() != nil || packet.Body() != nil {
		t.Fatalf("closed packet must expose only empty accessors")
	}
}
