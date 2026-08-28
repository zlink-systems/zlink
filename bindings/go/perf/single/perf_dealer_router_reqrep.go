package main

import (
	"context"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

func runDealerRouterReqRep(cfg benchmarkConfig) perfcommon.Result {
	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()

	replier, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer replier.Close()
	requester, err := ctx.DealerSocket()
	perfcommon.Must(err)
	defer requester.Close()
	replierMon := perfcommon.OpenMonitor(replier)
	defer replierMon.Close()
	requesterMon := perfcommon.OpenMonitor(requester)
	defer requesterMon.Close()

	perfcommon.Must(requester.SetRoutingID(zlink.NewRoutingID([]byte("DEALER-REQ"))))
	perfcommon.Must(perfcommon.ConfigureTLSServer(replier, cfg.transport))
	perfcommon.Must(perfcommon.ConfigureTLSClient(requester, cfg.transport))
	perfcommon.ApplySingleHWM(replier)
	perfcommon.ApplySingleHWM(requester)
	endpoint := perfcommon.BindAndResolveEndpoint(replier, cfg.transport, "perf-dealer-router-reqrep")
	perfcommon.Must(requester.Connect(endpoint))
	perfcommon.ApplySingleBenchmarkSocketOptions(replier, cfg.transport)
	perfcommon.ApplySingleBenchmarkSocketOptions(requester, cfg.transport)
	perfcommon.WaitConnectedWithActivity(
		perfcommon.SingleReadyTimeout(), replier, replierMon, requesterMon)

	result := runSingleReqRep(
		cfg,
		requester,
		replier,
		func() zlink.RequestOp { return requester.Request() },
		func(message *zlink.Message) (bool, error) {
			err := requester.Send().MoveMessage(message).Submit(context.Background())
			return err == nil, err
		},
	)
	perfcommon.PrintSingleAutoHWMDetail(replierMon, cfg.pattern, cfg.transport, "replier", zlink.SocketTypeRouter, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(requesterMon, cfg.pattern, cfg.transport, "requester", zlink.SocketTypeDealer, cfg.msgSize)
	return result
}
