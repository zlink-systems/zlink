package zlink_test

import (
	"bytes"
	"context"
	"net"
	"strings"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestSendConsumesMessageOwnership(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := inprocEndpoint("ownership-send")
	server, _ := ctx.PairSocket()
	client, _ := ctx.PairSocket()
	defer server.Close()
	defer client.Close()

	_ = server.Bind(endpoint)
	_ = client.Connect(endpoint)

	msg := newMessage(t, "owned")
	if err := client.Send().Message(msg).Submit(context.Background()); err != nil {
		t.Fatalf("Send() error = %v", err)
	}
	if data := msg.Data(); data != nil {
		t.Fatalf("moved message data should be nil, got %q", string(data))
	}
}

func TestRecvOwnershipCanBeExplicitlyReleased(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := inprocEndpoint("ownership-recv")
	server, _ := ctx.PairSocket()
	client, _ := ctx.PairSocket()
	defer server.Close()
	defer client.Close()

	_ = server.Bind(endpoint)
	_ = client.Connect(endpoint)
	_ = client.Send().Message(newMessage(t, "recv-owned")).Submit(context.Background())

	var received zlink.Received
	if _, err := server.Recv(&received, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	if err := received.Close(); err != nil {
		t.Fatalf("Received.Close() error = %v", err)
	}
}

func TestUnsentMessageSupportsExplicitClose(t *testing.T) {
	msg, err := zlink.NewMessage([]byte("unsent"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	if err := msg.Close(); err != nil {
		t.Fatalf("Message.Close() error = %v", err)
	}
}

func TestStreamRawAndPacketPullShapesPreservePayload(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	directServer, _ := ctx.StreamSocket()
	defer directServer.Close()
	if err := directServer.SetReceiveMode(zlink.StreamReceiveRaw); err != nil {
		t.Fatalf("direct SetReceiveMode() error = %v", err)
	}

	directEndpoint := tcpEndpoint(t)
	if err := directServer.Bind(directEndpoint); err != nil {
		t.Fatalf("direct Bind() error = %v", err)
	}
	directConn, err := net.DialTimeout("tcp", strings.TrimPrefix(directEndpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("direct dial error = %v", err)
	}
	defer directConn.Close()

	payload := []byte("frame-a/frame-b")
	if _, err := directConn.Write(payload); err != nil {
		t.Fatalf("direct Write() error = %v", err)
	}

	var directReceived zlink.Received
	if _, err := directServer.Recv(&directReceived, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("direct Recv() error = %v", err)
	}
	defer directReceived.Close()

	packetServer, _ := ctx.StreamSocket()
	defer packetServer.Close()
	if err := packetServer.SetReceiveMode(zlink.StreamReceivePacket); err != nil {
		t.Fatalf("packet SetReceiveMode() error = %v", err)
	}

	packetEndpoint := tcpEndpoint(t)
	if err := packetServer.Bind(packetEndpoint); err != nil {
		t.Fatalf("packet Bind() error = %v", err)
	}

	packetConn, err := net.DialTimeout("tcp", strings.TrimPrefix(packetEndpoint, "tcp://"), 5*time.Second)
	if err != nil {
		t.Fatalf("packet dial error = %v", err)
	}
	defer packetConn.Close()

	writeStreamPacket(t, packetConn, payload)
	var packet zlink.StreamPacket
	if ok, err := packetServer.RecvPacket(&packet, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("RecvPacket() = (%v, %v)", ok, err)
	}
	defer packet.Close()

	directPart, err := directReceived.SinglePartOrError()
	if err != nil {
		t.Fatalf("direct SinglePartOrError() error = %v", err)
	}
	if !bytes.Equal(directPart.Data(), packet.Body().Data()) {
		t.Fatalf("payload mismatch: raw=%q packet=%q", string(directPart.Data()), string(packet.Body().Data()))
	}
}
