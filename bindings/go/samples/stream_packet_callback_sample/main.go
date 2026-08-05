package main

import (
	"context"
	"fmt"
	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/samples/internal/samplecommon"
)

func main() {
	// --8<-- [start:doc]
	ctx, err := zlink.NewContext()
	samplecommon.Must(err)
	defer ctx.Close()

	server, err := ctx.StreamSocket()
	samplecommon.Must(err)
	defer server.Close()

	endpoint := samplecommon.UniqueTCP("stream-packet-callback")
	samplecommon.Must(server.Bind(endpoint))

	done := make(chan error, 1)
	samplecommon.Must(server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		if got := string(body.Data()); got != "hello-stream" {
			done <- fmt.Errorf("unexpected packet body %q", got)
			_ = header.Close()
			_ = body.Close()
			return
		}
		packet := samplecommon.FrameStreamPacketMessage(header, body)
		_ = header.Close()
		_ = body.Close()
		if _, err := server.SendTo(source).Message(packet).Submit(context.Background()); err != nil {
			_ = packet.Close()
			done <- err
			return
		}
		done <- nil
	}))

	conn := samplecommon.DialEndpoint(endpoint)
	defer conn.Close()

	sent := "hello-stream"
	samplecommon.WriteStreamPacket(conn, []byte(sent))

	buffer := samplecommon.ReadStreamPacketBody(conn)
	samplecommon.Must(<-done)

	fmt.Printf("[stream/packet-callback] send: %q -> recv: %q\n", sent, string(buffer))
	// --8<-- [end:doc]
}
