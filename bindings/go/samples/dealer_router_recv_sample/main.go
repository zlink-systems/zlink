package main

import (
	"bytes"
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

	router, err := ctx.RouterSocket()
	samplecommon.Must(err)
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	samplecommon.Must(err)
	defer dealer.Close()

	routerMon := samplecommon.OpenMonitor(router)
	defer routerMon.Close()
	dealerMon := samplecommon.OpenMonitor(dealer)
	defer dealerMon.Close()

	endpoint := samplecommon.UniqueTCP("dealer-router-recv")
	rid := zlink.NewRoutingID([]byte("dealer-sample"))
	samplecommon.Must(router.Bind(endpoint))
	samplecommon.Must(dealer.SetRoutingID(rid))
	samplecommon.Must(dealer.Connect(endpoint))
	samplecommon.WaitConnected(routerMon, dealerMon)

	_, err = dealer.Send().Message(samplecommon.Message("ping")).Submit(context.Background())
	samplecommon.Must(err)

	var request zlink.Received
	_, err = router.Recv(&request, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	defer request.Close()
	_, err = request.Send().Message(samplecommon.Message("pong")).Submit(context.Background())
	samplecommon.Must(err)

	var reply zlink.Received
	_, err = dealer.Recv(&reply, zlink.RecvFlagsNone)
	samplecommon.Must(err)
	defer reply.Close()
	part, err := reply.SinglePartOrError()
	samplecommon.Must(err)
	if !bytes.Equal(part.Data(), []byte("pong")) {
		samplecommon.Must(fmt.Errorf("unexpected reply %q", string(part.Data())))
	}

	fmt.Printf("[dealer-router/recv] send: %q -> recv: %q\n", "ping", string(part.Data()))
	// --8<-- [end:doc]
}
