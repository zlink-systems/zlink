package main

import (
	"context"
	"fmt"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type dealerRouterClient struct {
	socket  *zlink.DealerSocket
	monitor *zlink.SocketMonitor
}

func runMultiDealerRouterServer(cfg multiConfig) {
	serverCtx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)
	defer serverCtx.Close()

	router, err := serverCtx.RouterSocket()
	perfcommon.Must(err)
	defer router.Close()

	perfcommon.Must(perfcommon.ConfigureTLSServer(router, cfg.transport))
	perfcommon.ApplyMultiHWM(router, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(router, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(router, cfg.transport, "perf-multi-dealer-router")
	// PERF_MULTI_TEST_POLICY § 1.6: same recalculation point as the C relay
	// server (bindings/c/perf/multi/common/perf_multi_relay_server.hpp:658).
	perfcommon.Must(serverCtx.RecalculateAutoHwm())
	flushControlLine("READY,%s", endpoint)

	serverDone := make(chan struct{})
	stopSignal := waitForStopAsync()
	go startMultiRouterRouterEchoServer(router, stopSignal, serverDone)
	select {
	case <-serverDone:
	case <-stopSignal:
		// The echo loop observes the same control signal and leaves its poller
		// before the deferred socket/context close runs.
		<-serverDone
	}
}

// runMultiDealerRouterClient continuously submits until socket HWM applies
// backpressure and drains echoed messages independently on POLLIN.
func runMultiDealerRouterClient(cfg multiConfig, endpoint string) perfcommon.Result {
	clientCtx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer clientCtx.Close()

	dealers := make([]dealerRouterClient, 0, cfg.clients)
	for i := 0; i < cfg.clients; i++ {
		dealer, err := clientCtx.DealerSocket()
		perfcommon.Must(err)
		dealerMon := perfcommon.OpenMonitor(dealer)
		perfcommon.Must(perfcommon.ConfigureTLSClient(dealer, cfg.transport))
		perfcommon.ApplyMultiHWM(dealer, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(dealer, cfg.transport)
		rid := zlink.NewRoutingID([]byte(fmt.Sprintf("dealer-%06d", i)))
		perfcommon.Must(dealer.SetRoutingID(rid))
		perfcommon.Must(dealer.Connect(endpoint))
		dealers = append(dealers, dealerRouterClient{socket: dealer, monitor: dealerMon})
	}
	for _, dealer := range dealers {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), dealer.monitor)
	}
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate after target connections ready.
	perfcommon.Must(clientCtx.RecalculateAutoHwm())
	defer func() {
		for _, dealer := range dealers {
			_ = dealer.monitor.Close()
			_ = dealer.socket.Close()
		}
	}()

	targets := make([]multiEchoSocket, len(dealers))
	for i, dealer := range dealers {
		targets[i] = dealer.socket
	}
	result := runMultiEcho(cfg, targets, func(index int, message *zlink.Message) (zlink.SendSubmission, error) {
		return perfcommon.SubmitMeasurementSendSubmission(context.Background(), dealers[index].socket.Send(), message)
	})
	if len(dealers) > 0 {
		perfcommon.PrintMultiSocketAutoHWMDetail(
			dealers[0].socket, dealers[0].monitor, cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeDealer, cfg.msgSize,
		)
	}
	return result
}
