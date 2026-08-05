package main

import (
	"context"
	"fmt"
	"time"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
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
	perfcommon.ApplyMultiAutoHWMMsgUnit(serverCtx, cfg.msgSize)
	perfcommon.ApplyMultiHWM(router, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(router, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(router, cfg.transport, "perf-multi-dealer-router")
	flushControlLine("READY,%s", endpoint)

	serverDone := make(chan struct{})
	go startMultiRouterRouterEchoServer(router, serverDone)
	select {
	case <-serverDone:
	case <-waitForStopAsync():
	}
}

// runMultiDealerRouterClient mirrors perf_multi_client_helpers.hpp
// run_echo_duration / run_echo_window_round_robin: exactly one
// outstanding request per dealer (awaiting_reply / send_pending), a
// single poller per socket arming POLLIN while awaiting a reply and
// POLLOUT while a send is pending, signal-driven wait (-1), and RTT/2
// latency. The phase ends purely on the active deadline (the C echo
// client emits no stop token; the runner stops the server).
func runMultiDealerRouterClient(cfg multiConfig, endpoint string) perfcommon.Result {
	stats := perfcommon.NewStats()
	clientCtx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer clientCtx.Close()
	perfcommon.ApplyMultiAutoHWMMsgUnit(clientCtx, cfg.msgSize)

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
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), dealerMon)
		dealers = append(dealers, dealerRouterClient{socket: dealer, monitor: dealerMon})
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
	awaitingReply := make([]bool, len(dealers))
	sendPending := make([]bool, len(dealers))
	for i, dealer := range dealers {
		perfcommon.Must(poller.AddSocket(dealer.socket, perfcommon.ZLinkPollOut, uintptr(i)))
		payloads[i] = perfcommon.PreparePayload(msgSize)
	}

	for time.Now().Before(window.StopAt) {
		for i := range dealers {
			if awaitingReply[i] || sendPending[i] {
				continue
			}
			if sendMultiDealerRouterRequest(dealers[i].socket, payloads[i], msgSize, window) {
				awaitingReply[i] = true
				perfcommon.Must(poller.ModifySocket(dealers[i].socket, perfcommon.ZLinkPollIn))
			} else {
				sendPending[i] = true
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
			if events[i].Revents&perfcommon.ZLinkPollIn != 0 && awaitingReply[idx] {
				if recvMultiDealerRouterReply(socket, stats, msgSize, window) {
					awaitingReply[idx] = false
					perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollOut))
				}
			}
			if events[i].Revents&perfcommon.ZLinkPollOut != 0 && sendPending[idx] {
				if sendMultiDealerRouterRequest(socket, payloads[idx], msgSize, window) {
					sendPending[idx] = false
					awaitingReply[idx] = true
					perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollIn))
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
	sent, err := perfcommon.SubmitPayload(payload, func(message *zlink.Message) (bool, error) {
		return socket.Send().MoveMessage(message).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
	})
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false
		}
		perfcommon.Must(fmt.Errorf("multi dealer/router client send: %w", err))
	}
	return sent
}

func recvMultiDealerRouterReply(
	socket *zlink.DealerSocket,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) bool {
	var reply zlink.Received
	ok, err := socket.Recv(&reply, zlink.RecvFlagsDontWait)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false
		}
		perfcommon.Must(fmt.Errorf("multi dealer/router client recv: %w", err))
	}
	if !ok {
		return false
	}
	part, partErr := reply.SinglePartOrError()
	if partErr == nil {
		perfcommon.RecordMessageRTTLatency(stats, window.ActiveAt, window.StopAt, msgSize, part)
	}
	_ = reply.Close()
	return true
}
