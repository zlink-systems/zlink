package main

import (
	"bytes"
	"context"
	"fmt"
	"time"
	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/samples/internal/samplecommon"
)

func main() {
	// --8<-- [start:doc]
	ctx, err := zlink.NewContext()
	samplecommon.Must(err)
	defer ctx.Close()

	routerSocket, err := ctx.RouterSocket()
	samplecommon.Must(err)
	defer routerSocket.Close()
	dealerSocket, err := ctx.DealerSocket()
	samplecommon.Must(err)
	defer dealerSocket.Close()

	routerMon := samplecommon.OpenMonitor(routerSocket)
	defer routerMon.Close()
	dealerMon := samplecommon.OpenMonitor(dealerSocket)
	defer dealerMon.Close()

	endpoint := samplecommon.UniqueTCP("request-reply-async")
	rid := zlink.NewRoutingID([]byte("request-reply-client"))
	samplecommon.Must(routerSocket.Bind(endpoint))
	samplecommon.Must(dealerSocket.SetRoutingID(rid))
	samplecommon.Must(dealerSocket.Connect(endpoint))
	samplecommon.WaitConnected(routerMon, dealerMon)

	requestDone := make(chan error, 1)
	go func() {
		var received zlink.Received
		_, err := routerSocket.Recv(&received, zlink.RecvFlagsNone)
		if err != nil {
			requestDone <- err
			return
		}
		defer received.Close()
		part, err := received.SinglePartOrError()
		if err != nil {
			requestDone <- err
			return
		}
		if received.RoutingID().String() != rid.String() {
			requestDone <- fmt.Errorf("unexpected routing id %q", received.RoutingID().String())
			return
		}
		if !bytes.Equal(part.Data(), []byte("ping")) {
			requestDone <- fmt.Errorf("unexpected request %q", string(part.Data()))
			return
		}
		token, ok := received.ReplyToken()
		if !ok {
			requestDone <- fmt.Errorf("missing reply token")
			return
		}
		replyErr := routerSocket.Reply(received.RoutingID(), token).Message(samplecommon.Message("pong")).Submit(context.Background())
		requestDone <- replyErr
	}()

	reply, err := dealerSocket.Request().
		Message(samplecommon.Message("ping")).
		Timeout(2 * time.Second).
		Submit(context.Background())
	samplecommon.Must(err)
	defer func() {
		for _, part := range reply {
			part.Close()
		}
	}()

	if len(reply) != 1 {
		samplecommon.Must(fmt.Errorf("unexpected reply part count %d", len(reply)))
	}
	if !bytes.Equal(reply[0].Data(), []byte("pong")) {
		samplecommon.Must(fmt.Errorf("unexpected reply %q", string(reply[0].Data())))
	}
	samplecommon.Must(<-requestDone)

	fmt.Printf("[dealer-router/request-reply/async] send: %q -> recv: %q\n", "ping", string(reply[0].Data()))
	// --8<-- [end:doc]
}
