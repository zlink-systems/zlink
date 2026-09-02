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

	zlink "zlink.systems/zlink"
)

func TestRuntimeVersionIsAvailable(t *testing.T) {
	version := zlink.RuntimeVersion()
	if version.Major < 0 || version.Minor < 0 || version.Patch < 0 {
		t.Fatalf("RuntimeVersion() returned an invalid version: %d.%d.%d", version.Major, version.Minor, version.Patch)
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
	want := fmt.Sprint(zlink.RuntimeVersion().Patch)
	if got := lastNonEmptyLine(string(output)); got != want {
		t.Fatalf("ZLINK_VERSION_PATCH from direct zlink/common.h include = %q, want %s", got, want)
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
		if _, ok := received.ReplyToken(); !ok {
			t.Errorf("ReplyToken() = invalid")
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
	reply, err := dealerSocket.Request().Bytes(requestPayload).Timeout(2 * time.Second).Submit(context.Background())
	if !bytes.Equal(requestPayload, []byte("ping")) {
		t.Fatalf("Request().Bytes() mutated caller payload = %q", string(requestPayload))
	}
	if err != nil {
		t.Fatalf("Request() error = %v", err)
	}
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
	if err := dealerSocket.Send().Message(payload).Submit(context.Background()); err != nil {
		t.Fatalf("Send() error = %v", err)
	}
	var received zlink.Received
	if _, err := routerSocket.Recv(&received, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	defer received.Close()
	if token, ok := received.ReplyToken(); ok || token != (zlink.ReplyToken{}) {
		t.Fatalf("DATA ReplyToken() = (%v, %v), want (zero, false)", token, ok)
	}
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
	if err := stream.SetReceiveMode(zlink.StreamReceiveRaw); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}

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
	if err := stream.SendTo(received.RoutingID()).Message(reply).Submit(context.Background()); err != nil {
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

func TestStreamPacketPullCanonicalRoundTrip(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatalf("StreamSocket() error = %v", err)
	}
	defer stream.Close()
	if err := stream.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("SetReceiveMode() error = %v", err)
	}

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
	writeStreamPacket(t, conn, payload)
	var packet zlink.StreamPacket
	ok, err := stream.RecvPacket(&packet, zlink.RecvFlagsNone)
	if err != nil || !ok {
		t.Fatalf("RecvPacket() = ok %v, err %v", ok, err)
	}
	if got := string(packet.Body().Data()); got != "hello-stream" {
		t.Fatalf("packet body = %q, want %q", got, "hello-stream")
	}
	framed := frameStreamPacketMessage(t, packet.Header(), packet.Body())
	if err := stream.SendTo(packet.RoutingID()).Message(framed).Submit(context.Background()); err != nil {
		t.Fatalf("packet reply error = %v", err)
	}

	buffer := readStreamPacketBody(t, conn)
	if !bytes.Equal(buffer, payload) {
		t.Fatalf("stream packet reply = %q, want %q", string(buffer), string(payload))
	}
}
