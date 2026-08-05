package main

import (
	"context"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

func runDealerRouter(cfg benchmarkConfig) perfcommon.Result {
	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()
	perfcommon.ApplySingleAutoHWMMsgUnit(ctx, cfg.msgSize)

	router, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	perfcommon.Must(err)
	defer dealer.Close()
	routerMon := perfcommon.OpenMonitor(router)
	defer routerMon.Close()
	dealerMon := perfcommon.OpenMonitor(dealer)
	defer dealerMon.Close()

	rid := zlink.NewRoutingID([]byte("perf-dealer"))

	perfcommon.Must(perfcommon.ConfigureTLSServer(router, cfg.transport))
	perfcommon.Must(perfcommon.ConfigureTLSClient(dealer, cfg.transport))
	perfcommon.ApplySingleHWM(router)
	perfcommon.ApplySingleHWM(dealer)
	endpoint := perfcommon.BindAndResolveEndpoint(router, cfg.transport, "perf-dealer-router")
	perfcommon.Must(dealer.SetRoutingID(rid))
	perfcommon.Must(dealer.Connect(endpoint))
	perfcommon.ApplySingleBenchmarkSocketOptions(router, cfg.transport)
	perfcommon.ApplySingleBenchmarkSocketOptions(dealer, cfg.transport)
	perfcommon.WaitConnectedWithTimeout(perfcommon.SingleReadyTimeout(), routerMon, dealerMon)
	waitSingleRouteReady("dealer/router perf endpoint", func(payload []byte) error {
		_, err := perfcommon.SubmitMessage(perfcommon.NewMessage(payload), func(message *zlink.Message) (bool, error) {
			return dealer.Send().MoveMessage(message).Submit(context.Background())
		})
		return err
	}, router)

	result := runSingleRoutedOneWay(cfg, router, func(message *zlink.Message) (bool, error) {
		return dealer.Send().MoveMessage(message).Submit(context.Background())
	}, func(message *zlink.Message) error {
		_, err := dealer.Send().MoveMessage(message).Submit(context.Background())
		return err
	})
	perfcommon.PrintSingleAutoHWMDetail(routerMon, cfg.pattern, cfg.transport, "router", zlink.SocketTypeRouter, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(dealerMon, cfg.pattern, cfg.transport, "dealer", zlink.SocketTypeDealer, cfg.msgSize)
	return result
}
