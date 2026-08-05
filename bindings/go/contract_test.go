package zlink_test

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net"
	"os/exec"
	"strings"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

func TestRuntimeVersionIsAvailable(t *testing.T) {
	version := zlink.RuntimeVersion()
	if version.Major <= 0 {
		t.Fatalf("RuntimeVersion().Major = %d, want > 0", version.Major)
	}
}

func TestCoreResultAndEventConstantsArePublic(t *testing.T) {
	if zlink.RequestBackpressured != zlink.RequestResult(113) {
		t.Fatalf("RequestBackpressured = %d, want 113", zlink.RequestBackpressured)
	}
	if zlink.RecvBufferTooSmall != zlink.RecvResult(207) || zlink.RecvInvalidState != zlink.RecvResult(208) {
		t.Fatalf("receive result constants = (%d, %d), want (207, 208)", zlink.RecvBufferTooSmall, zlink.RecvInvalidState)
	}
	if zlink.ConnectAuthFailed != zlink.ConnectResult(608) {
		t.Fatalf("ConnectAuthFailed = %d, want 608", zlink.ConnectAuthFailed)
	}
	if zlink.ConfigConflict != zlink.ConfigResult(707) || zlink.ConfigBufferTooSmall != zlink.ConfigResult(708) || zlink.ConfigBusy != zlink.ConfigResult(709) {
		t.Fatalf("config result constants = (%d, %d, %d), want (707, 708, 709)", zlink.ConfigConflict, zlink.ConfigBufferTooSmall, zlink.ConfigBusy)
	}
	if zlink.MonitorEventHandshakeFailedAuth != zlink.MonitorEventMask(1<<14) || zlink.MonitorEventTypeHandshakeFailedAuth != zlink.MonitorEventType(1<<14) {
		t.Fatalf("authentication monitor constants = (%#x, %#x), want (%#x, %#x)", zlink.MonitorEventHandshakeFailedAuth, zlink.MonitorEventTypeHandshakeFailedAuth, 1<<14, 1<<14)
	}
	if zlink.PollErr != zlink.PollEventFlag(4) || zlink.PollPri != zlink.PollEventFlag(8) {
		t.Fatalf("poll error flags = (%d, %d), want (4, 8)", zlink.PollErr, zlink.PollPri)
	}
}

func TestDirectCommonHeaderVersionMatchesPackage(t *testing.T) {
	cmd := exec.Command("cc", "-E", "-Iinclude", "-x", "c", "-")
	cmd.Stdin = strings.NewReader("#include <zlink/common.h>\nZLINK_VERSION_PATCH\n")
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("preprocess zlink/common.h: %v\n%s", err, output)
	}
	if got := lastNonEmptyLine(string(output)); got != "0" {
		t.Fatalf("ZLINK_VERSION_PATCH from direct zlink/common.h include = %q, want 0", got)
	}
}

func lastNonEmptyLine(output string) string {
	lines := strings.Split(output, "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if line := strings.TrimSpace(lines[i]); line != "" {
			return line
		}
	}
	return ""
}

func TestContextLifecycle(t *testing.T) {
	ctx := newContext(t)
	if _, err := ctx.Options().IOThreads(); err != nil {
		t.Fatalf("IOThreads() error = %v", err)
	}
	if err := ctx.Shutdown(); err != nil {
		t.Fatalf("Shutdown() error = %v", err)
	}
	if err := ctx.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
}

func TestRequestReplyCanonicalDealerRouterRoundTrip(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	routerSocket, err := ctx.RouterSocket()
	if err != nil {
		t.Fatalf("RouterSocket() error = %v", err)
	}
	defer routerSocket.Close()

	dealerSocket, err := ctx.DealerSocket()
	if err != nil {
		t.Fatalf("DealerSocket() error = %v", err)
	}
	defer dealerSocket.Close()

	endpoint := inprocEndpoint("request-reply")
	if err := routerSocket.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealerSocket.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	done := make(chan struct{})
	go func() {
		defer close(done)
		var received zlink.Received
		if _, err := routerSocket.Recv(&received, zlink.RecvFlagsNone); err != nil {
			t.Errorf("Recv() error = %v", err)
			return
		}
		defer received.Close()
		if !received.HasRoutingID() {
			t.Errorf("HasRoutingID() = false")
			return
		}
		if !received.HasRequestSeq() {
			t.Errorf("HasRequestSeq() = false")
			return
		}
		if !received.IsSinglePart() {
			t.Errorf("IsSinglePart() = false")
			return
		}
		part, err := received.FirstPart()
		if err != nil {
			t.Errorf("FirstPart() error = %v", err)
			return
		}
		if got := string(part.Data()); got != "ping" {
			t.Errorf("request payload = %q, want %q", got, "ping")
			return
		}
		reply, err := zlink.NewMessage([]byte("pong"))
		if err != nil {
			t.Errorf("NewMessage() error = %v", err)
			return
		}
		if err := received.Reply().Message(reply).Submit(context.Background()); err != nil {
			t.Errorf("Received.Reply() error = %v", err)
		}
	}()

	requestPayload := []byte("ping")
	replyCh, err := dealerSocket.Request().Bytes(requestPayload).Timeout(2 * time.Second).SubmitAsync(context.Background())
	if err != nil {
		t.Fatalf("Request() error = %v", err)
	}
	if !bytes.Equal(requestPayload, []byte("ping")) {
		t.Fatalf("Request().Bytes() mutated caller payload = %q", string(requestPayload))
	}
	completion := <-replyCh
	if completion.Err != nil {
		t.Fatalf("Request() completion error = %v", completion.Err)
	}
	reply := completion.Parts
	if len(reply) != 1 {
		t.Fatalf("Request() reply parts = %d, want 1", len(reply))
	}
	defer reply[0].Close()
	part := reply[0]
	if got := string(part.Data()); got != "pong" {
		t.Fatalf("reply payload = %q, want %q", got, "pong")
	}
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatalf("request handler did not run")
	}
}

func TestRouterRequestSupportPreservesDataReceiveSurface(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	routerSocket, err := ctx.RouterSocket()
	if err != nil {
		t.Fatalf("RouterSocket() error = %v", err)
	}
	defer routerSocket.Close()

	dealerSocket, err := ctx.DealerSocket()
	if err != nil {
		t.Fatalf("DealerSocket() error = %v", err)
	}
	defer dealerSocket.Close()

	endpoint := inprocEndpoint("request-reply-data")
	if err := routerSocket.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealerSocket.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	payload, err := zlink.NewMessage([]byte("plain-data"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	if _, err := dealerSocket.Send().Message(payload).Submit(context.Background()); err != nil {
		t.Fatalf("Send() error = %v", err)
	}
	var received zlink.Received
	if _, err := routerSocket.Recv(&received, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	defer received.Close()
	part, err := received.SinglePartOrError()
	if err != nil {
		t.Fatalf("SinglePartOrError() error = %v", err)
	}
	if got := string(part.Data()); got != "plain-data" {
		t.Fatalf("data payload = %q, want %q", got, "plain-data")
	}
}

func TestStreamRecvCanonicalRoundTrip(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatalf("StreamSocket() error = %v", err)
	}
	defer stream.Close()

	monitor, err := zlink.OpenSocketMonitor(
		stream,
		zlink.MonitorEventAll,
	)
	if err != nil {
		t.Fatalf("OpenSocketMonitor() error = %v", err)
	}
	defer monitor.Close()

	endpoint := tcpEndpoint(t)
	if err := stream.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	waitForMonitorEvent(t, monitor, 5*time.Second)

	payload := []byte("hello-stream")
	if _, err := conn.Write(payload); err != nil {
		t.Fatalf("conn.Write() error = %v", err)
	}

	var received zlink.Received
	if _, err := stream.Recv(&received, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	defer received.Close()
	part, err := received.SinglePartOrError()
	if err != nil {
		t.Fatalf("SinglePartOrError() error = %v", err)
	}
	if !bytes.Equal(part.Data(), payload) {
		t.Fatalf("stream payload = %q, want %q", string(part.Data()), string(payload))
	}

	reply := newMessage(t, "hello-stream")
	if _, err := stream.SendTo(received.RoutingID()).Message(reply).Submit(context.Background()); err != nil {
		t.Fatalf("SendTo() error = %v", err)
	}

	buffer := make([]byte, len(payload))
	if _, err := io.ReadFull(conn, buffer); err != nil {
		t.Fatalf("io.ReadFull() error = %v", err)
	}
	if !bytes.Equal(buffer, payload) {
		t.Fatalf("stream reply = %q, want %q", string(buffer), string(payload))
	}
}

func TestStreamOnPacketCanonicalRoundTrip(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatalf("StreamSocket() error = %v", err)
	}
	defer stream.Close()

	monitor, err := zlink.OpenSocketMonitor(
		stream,
		zlink.MonitorEventAll,
	)
	if err != nil {
		t.Fatalf("OpenSocketMonitor() error = %v", err)
	}
	defer monitor.Close()

	endpoint := tcpEndpoint(t)
	if err := stream.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	done := make(chan error, 1)
	if err := stream.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		if got := string(body.Data()); got != "hello-stream" {
			done <- fmt.Errorf("packet body = %q, want %q", got, "hello-stream")
			_ = header.Close()
			_ = body.Close()
			return
		}
		packet := frameStreamPacketMessage(t, header, body)
		_ = header.Close()
		_ = body.Close()
		if _, err := stream.SendTo(source).Message(packet).Submit(context.Background()); err != nil {
			_ = packet.Close()
			done <- err
			return
		}
		done <- nil
	}); err != nil {
		t.Fatalf("OnPacket() error = %v", err)
	}

	conn, err := net.DialTimeout("tcp", strings.TrimPrefix(endpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("net.DialTimeout() error = %v", err)
	}
	defer conn.Close()

	waitForMonitorEvent(t, monitor, 5*time.Second)

	payload := []byte("hello-stream")
	writeStreamPacket(t, conn, payload)

	buffer := readStreamPacketBody(t, conn)
	if !bytes.Equal(buffer, payload) {
		t.Fatalf("stream callback reply = %q, want %q", string(buffer), string(payload))
	}

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("callback send error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatalf("stream callback did not reply")
	}
}
