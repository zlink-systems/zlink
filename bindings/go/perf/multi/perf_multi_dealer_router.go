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
	defer func() {
		for _, dealer := range dealers {
			_ = dealer.monitor.Close()
			_ = dealer.socket.Close()
		}
	}()

	window := activeDeadline(cfg.duration)
	runMultiDealerRouterEchoWindow(dealers, stats, cfg.msgSize, window)
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
	payloads := make([][]byte, len(dealers))
	sendPending := make([]bool, len(dealers))
	for i, dealer := range dealers {
		perfcommon.Must(poller.AddSocket(dealer.socket, perfcommon.ZLinkPollIn, uintptr(i)))
		payloads[i] = perfcommon.PreparePayload(msgSize)
	}

	for time.Now().Before(window.StopAt) {
		for i := range dealers {
			if sendPending[i] {
				continue
			}
			for time.Now().Before(window.StopAt) && sendMultiDealerRouterRequest(dealers[i].socket, payloads[i], msgSize, window) {
			}
			if time.Now().Before(window.StopAt) {
				sendPending[i] = true
				perfcommon.Must(poller.ModifySocket(dealers[i].socket, perfcommon.ZLinkPollIn|perfcommon.ZLinkPollOut))
			}
		}

		if !time.Now().Before(window.StopAt) {
			break
		}
		wait := time.Until(window.StopAt)
		if wait <= 0 {
			break
		}
		n, waitErr := poller.Wait(events, wait)
		if waitErr != nil {
			if perfcommon.IsTransient(waitErr) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi dealer/router client poll: %w", waitErr))
		}
		for i := 0; i < n; i++ {
			idx := int(events[i].Slot)
			if idx < 0 || idx >= len(dealers) {
				continue
			}
			socket := dealers[idx].socket
			if events[i].Revents&perfcommon.ZLinkPollIn != 0 {
				drainMultiDealerRouterReplies(socket, stats, msgSize, window)
			}
			if events[i].Revents&perfcommon.ZLinkPollOut != 0 && sendPending[idx] {
				sendPending[idx] = false
				perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollIn))
				for time.Now().Before(window.StopAt) && sendMultiDealerRouterRequest(socket, payloads[idx], msgSize, window) {
				}
				if time.Now().Before(window.StopAt) {
					sendPending[idx] = true
					perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollIn|perfcommon.ZLinkPollOut))
				}
			}
		}
	}
}

func sendMultiDealerRouterRequest(
	socket *zlink.DealerSocket,
	payload []byte,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) bool {
	perfcommon.StampWindowPayload(payload, window.ActiveAt)
	message := perfcommon.NewMessage(payload)
	err := perfcommon.SubmitMeasurementRoutedFlags(socket.Send(), message, zlink.SendFlagsDontWait)
	sent := err == nil
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false
		}
		perfcommon.Must(fmt.Errorf("multi dealer/router client send: %w", err))
	}
	return sent
}

func drainMultiDealerRouterReplies(
	socket *zlink.DealerSocket,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) {
	var reply zlink.Received
	for {
		ok, err := socket.Recv(&reply, zlink.RecvFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				return
			}
			perfcommon.Must(fmt.Errorf("multi dealer/router client recv: %w", err))
		}
		if !ok {
			return
		}
		part, partErr := perfcommon.MeasurementPayload(reply.Parts())
		if partErr == nil {
			perfcommon.RecordMessageRTTLatency(stats, window.ActiveAt, window.StopAt, msgSize, part)
		}
		_ = reply.Close()
	}
}
