package main

import (
	"fmt"
	"time"

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
	stats := perfcommon.NewMultiStats()
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

	window := activeDeadline(cfg.duration)
	runMultiDealerRouterEchoWindow(dealers, stats, cfg.msgSize, window)
	if len(dealers) > 0 {
		perfcommon.PrintMultiSocketAutoHWMDetail(
			dealers[0].socket, dealers[0].monitor, cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeDealer, cfg.msgSize,
		)
	}
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func runMultiDealerRouterEchoWindow(
	dealers []dealerRouterClient,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) {
	if len(dealers) == 0 {
		return
	}
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	events := make([]zlink.PollEvent, len(dealers))
	for i, dealer := range dealers {
		perfcommon.Must(poller.AddSocket(
			dealer.socket, perfcommon.ZLinkPollIn|zlink.PollCompletion, uintptr(i)))
	}

	payloads := make([][]byte, len(dealers))
	for index := range payloads {
		payloads[index] = perfcommon.PreparePayload(msgSize)
	}
	submit := func(index int) error {
		if sendErr := sendMultiDealerRouterRequest(
			dealers[index].socket, payloads[index], window); sendErr != nil {
			return fmt.Errorf("multi dealer/router client send: %w", sendErr)
		}
		return nil
	}
	pendingReplies := 0
	progress := func(wait time.Duration) error {
		n, waitErr := poller.Wait(events, wait)
		if waitErr != nil {
			if perfcommon.IsTransient(waitErr) {
				return nil
			}
			return fmt.Errorf("multi dealer/router client poll: %w", waitErr)
		}
		for i := 0; i < n; i++ {
			idx := int(events[i].Slot)
			if idx < 0 || idx >= len(dealers) {
				continue
			}
			socket := dealers[idx].socket
			if events[i].Revents&perfcommon.ZLinkPollIn != 0 {
				drained := drainMultiDealerRouterReplies(socket, stats, msgSize, window)
				if drained > pendingReplies {
					return fmt.Errorf("multi dealer/router received %d replies with %d pending", drained, pendingReplies)
				}
				pendingReplies -= drained
			}
		}
		return nil
	}
	perfcommon.Must(runMultiSendTurns(
		len(dealers), window, "multi dealer/router", submit, progress,
		func(submitted int) { pendingReplies += submitted },
		func() bool { return pendingReplies > 0 }))
}

func sendMultiDealerRouterRequest(
	socket *zlink.DealerSocket,
	payload []byte,
	window perfcommon.BenchmarkWindow,
) error {
	perfcommon.StampWindowPayload(payload, window.ActiveAt)
	message := perfcommon.NewMessage(payload)
	return perfcommon.SubmitMeasurementSend(socket.Send(), message)
}

func drainMultiDealerRouterReplies(
	socket *zlink.DealerSocket,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) int {
	drained := 0
	var reply zlink.Received
	for {
		ok, err := socket.Recv(&reply, zlink.RecvFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				return drained
			}
			perfcommon.Must(fmt.Errorf("multi dealer/router client recv: %w", err))
		}
		if !ok {
			return drained
		}
		part, partErr := perfcommon.MeasurementPayload(reply.Parts())
		if partErr == nil {
			drained++
			perfcommon.RecordMessageRTTLatency(stats, window.ActiveAtNs, window.StopAtNs, msgSize, part)
		}
		_ = reply.Close()
	}
}
