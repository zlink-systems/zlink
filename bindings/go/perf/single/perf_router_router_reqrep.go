package main

import (
	"context"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

func runRouterRouterReqRep(cfg benchmarkConfig) perfcommon.Result {
	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()

	replier, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer replier.Close()
	requester, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer requester.Close()
	replierMon := perfcommon.OpenMonitor(replier)
	defer replierMon.Close()
	requesterMon := perfcommon.OpenMonitor(requester)
	defer requesterMon.Close()

	replierID := zlink.NewRoutingID([]byte("SERVER"))
	requesterID := zlink.NewRoutingID([]byte("CLIENT"))
	perfcommon.Must(perfcommon.ConfigureTLSServer(replier, cfg.transport))
	perfcommon.Must(perfcommon.ConfigureTLSClient(requester, cfg.transport))
	perfcommon.ApplySingleHWM(replier)
	perfcommon.ApplySingleHWM(requester)
	endpoint := perfcommon.BindAndResolveEndpoint(replier, cfg.transport, "perf-router-router-reqrep")
	perfcommon.Must(replier.SetRoutingID(replierID))
	perfcommon.Must(requester.SetRoutingID(requesterID))
	perfcommon.Must(replier.SetMandatory(true))
	perfcommon.Must(requester.SetMandatory(true))
	perfcommon.Must(requester.SetConnectRoutingID(replierID))
	perfcommon.Must(requester.Connect(endpoint))
	perfcommon.ApplySingleBenchmarkSocketOptions(replier, cfg.transport)
	perfcommon.ApplySingleBenchmarkSocketOptions(requester, cfg.transport)
	perfcommon.WaitConnectedWithActivity(
		perfcommon.SingleReadyTimeout(), replier, replierMon, requesterMon)
	targetID := waitRouterRouterRouteReady(replier, requester, replierID)

	result := runSingleReqRep(
		cfg,
		requester,
		replier,
		func() zlink.RequestOp { return requester.Request(targetID) },
		func(message *zlink.Message) (bool, error) {
			err := requester.SendTo(targetID).MoveMessage(message).Submit(context.Background())
			return err == nil, err
		},
	)
	perfcommon.PrintSingleAutoHWMDetail(replierMon, cfg.pattern, cfg.transport, "replier", zlink.SocketTypeRouter, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(requesterMon, cfg.pattern, cfg.transport, "requester", zlink.SocketTypeRouter, cfg.msgSize)
	return result
}
