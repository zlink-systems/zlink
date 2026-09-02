package main

import (
	"context"
	"fmt"
	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/samples/internal/samplecommon"
)

func main() {
	// --8<-- [start:doc]
	ctx, err := zlink.NewContext()
	samplecommon.Must(err)
	defer ctx.Close()

	server, err := ctx.StreamSocket()
	samplecommon.Must(err)
	defer server.Close()
	samplecommon.Must(server.SetReceiveMode(zlink.StreamReceivePacket))

	endpoint := samplecommon.UniqueTCP("stream-packet-callback")
	samplecommon.Must(server.Bind(endpoint))

	conn := samplecommon.DialEndpoint(endpoint)
	defer conn.Close()

	sent := "hello-stream"
	samplecommon.WriteStreamPacket(conn, []byte(sent))
	var received zlink.StreamPacket
	ok, err := server.RecvPacket(&received, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	if !ok || string(received.Body().Data()) != sent {
		samplecommon.Must(fmt.Errorf("unexpected packet body"))
	}
	packet := samplecommon.FrameStreamPacketMessage(received.Header(), received.Body())
	samplecommon.Must(server.SendTo(received.RoutingID()).Message(packet).Submit(context.Background()))

	buffer := samplecommon.ReadStreamPacketBody(conn)

	fmt.Printf("[stream/packet-callback] send: %q -> recv: %q\n", sent, string(buffer))
	// --8<-- [end:doc]
}
