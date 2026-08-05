package main

import (
	"bytes"
	"context"
	"fmt"
	"time"
	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/samples/internal/samplecommon"
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
		requestSeq := received.RequestSeq()
		if !received.HasRequestSeq() {
			requestDone <- fmt.Errorf("missing request sequence")
			return
		}
		replyErr := routerSocket.Reply(received.RoutingID(), requestSeq).Message(samplecommon.Message("pong")).Submit(context.Background())
		requestDone <- replyErr
	}()

	replyCh := make(chan []*zlink.Message, 1)
	errCh := make(chan error, 1)
	go func() {
		completions, err := dealerSocket.Request().Message(samplecommon.Message("ping")).Timeout(2 * time.Second).SubmitAsync(context.Background())
		if err != nil {
			errCh <- err
			return
		}
		completion := <-completions
		if completion.Err != nil {
			errCh <- completion.Err
			return
		}
		replyCh <- completion.Parts
	}()

	var reply []*zlink.Message
	select {
	case err := <-errCh:
		samplecommon.Must(err)
	case reply = <-replyCh:
	}
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
