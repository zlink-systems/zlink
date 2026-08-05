package main

import (
	"bytes"
	"context"
	"fmt"
	"io"
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

	endpoint := samplecommon.UniqueTCP("stream-recv")
	samplecommon.Must(server.Bind(endpoint))

	conn := samplecommon.DialEndpoint(endpoint)
	defer conn.Close()

	sent := "hello-stream"
	_, err = conn.Write([]byte(sent))
	samplecommon.Must(err)

	var received zlink.Received
	_, err = server.Recv(&received, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	defer received.Close()
	part, err := received.SinglePartOrError()
	samplecommon.Must(err)
	if !bytes.Equal(part.Data(), []byte(sent)) {
		samplecommon.Must(fmt.Errorf("unexpected payload %q", string(part.Data())))
	}

	_, err = received.Send().Message(samplecommon.Message(sent)).Submit(context.Background())
	samplecommon.Must(err)

	buffer := make([]byte, len(sent))
	_, err = io.ReadFull(conn, buffer)
	samplecommon.Must(err)
	fmt.Printf("[stream/recv] send: %q -> recv: %q\n", sent, string(buffer))
	// --8<-- [end:doc]
}
