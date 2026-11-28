package main

import (
	"fmt"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/samples/internal/samplecommon"
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

	serverMon, err := zlink.OpenSocketMonitor(server, zlink.MonitorEventConnectionReady)
	samplecommon.Must(err)
	defer serverMon.Close()
	clientMon, err := zlink.OpenSocketMonitor(client, zlink.MonitorEventConnectionReady)
	samplecommon.Must(err)
	defer clientMon.Close()

	endpoint := samplecommon.UniqueTCP("monitor")
	samplecommon.Must(server.Bind(endpoint))
	samplecommon.Must(client.Connect(endpoint))

	poller, err := zlink.NewPoller()
	samplecommon.Must(err)
	defer poller.Close()

	monitors := []*zlink.SocketMonitor{serverMon, clientMon}
	for slot, monitor := range monitors {
		samplecommon.Must(poller.AddMonitor(monitor, zlink.PollIn, uintptr(slot)))
	}
	var events [2]zlink.PollEvent
	for pending := len(monitors); pending > 0; {
		n, err := poller.Wait(events[:], 5*time.Second)
		samplecommon.Must(err)
		if n == 0 {
			samplecommon.Must(fmt.Errorf("timed out waiting for CONNECTION_READY"))
		}
		for _, event := range events[:n] {
			monitor := monitors[event.Slot]
			ready, err := monitor.Recv(zlink.RecvFlagsDontWait)
			samplecommon.Must(err)
			if !ready.IsConnectionReady() {
				samplecommon.Must(fmt.Errorf("expected CONNECTION_READY"))
			}
			samplecommon.Must(poller.RemoveMonitor(monitor))
			pending--
		}
	}
	fmt.Println("[monitor/recv] recv: \"connection-ready\"")
}
