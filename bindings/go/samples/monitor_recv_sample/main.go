package main

import (
	"fmt"
	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/samples/internal/samplecommon"
)

func main() {
	ctx, err := zlink.NewContext()
	samplecommon.Must(err)
	defer ctx.Close()

	server, err := ctx.PairSocket()
	samplecommon.Must(err)
	defer server.Close()
	client, err := ctx.PairSocket()
	samplecommon.Must(err)
	defer client.Close()

	serverMon := samplecommon.OpenMonitor(server)
	defer serverMon.Close()
	clientMon := samplecommon.OpenMonitor(client)
	defer clientMon.Close()

	endpoint := samplecommon.UniqueTCP("monitor")
	samplecommon.Must(server.Bind(endpoint))
	samplecommon.Must(client.Connect(endpoint))

	serverEvent := samplecommon.WaitMonitorEvent(serverMon)
	clientEvent := samplecommon.WaitMonitorEvent(clientMon)
	if !serverEvent.IsConnectionReady() || !clientEvent.IsConnectionReady() {
		samplecommon.Must(fmt.Errorf("monitor sample expected CONNECTION_READY"))
	}
	fmt.Println("[monitor/recv] recv: \"connection-ready\"")
}
