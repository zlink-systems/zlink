package main

import (
	"context"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

func runDealerRouter(cfg benchmarkConfig) perfcommon.Result {
	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()

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
	// A ROUTER accepts the peer connection while processing socket activity.
	// Keep the readiness rule aligned with the C DEALER_ROUTER harness and the
	// ROUTER_ROUTER Go harness: drain that activity while polling both monitors.
	perfcommon.WaitConnectedWithActivity(
		perfcommon.SingleReadyTimeout(), router, routerMon, dealerMon)
	waitSingleRouteReady("dealer/router perf endpoint", func(payload []byte) error {
		_, err := perfcommon.SubmitRoutedMessage(perfcommon.NewMessage(payload), func(message *zlink.Message) <-chan error {
			return dealer.Send().MoveMessage(message).Submit(context.Background())
		})
		return err
	}, router)

	result := runSingleRoutedOneWay(cfg, router, func(message *zlink.Message) (bool, error) {
		return perfcommon.SubmitRoutedMessage(message, func(message *zlink.Message) <-chan error {
			return dealer.Send().MoveMessage(message).Submit(context.Background())
		})
	}, func(message *zlink.Message) error {
		return perfcommon.AwaitRoutedSend(dealer.Send().MoveMessage(message).Submit(context.Background()))
	})
	perfcommon.PrintSingleAutoHWMDetail(routerMon, cfg.pattern, cfg.transport, "router", zlink.SocketTypeRouter, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(dealerMon, cfg.pattern, cfg.transport, "dealer", zlink.SocketTypeDealer, cfg.msgSize)
	return result
}
