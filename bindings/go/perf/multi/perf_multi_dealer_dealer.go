package main

import (
	"context"
	"fmt"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type dealerDealerClient struct {
	ctx    *zlink.Context
	socket *zlink.DealerSocket
	mon    *zlink.SocketMonitor
}

func runMultiDealerDealerServer(cfg multiConfig) {
	serverCtx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)
	defer serverCtx.Close()

	server, err := serverCtx.DealerSocket()
	perfcommon.Must(err)
	defer server.Close()

	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-dealer-dealer")
	flushControlLine("READY,%s", endpoint)
	if !waitForStartToken(cfg.msgSize) {
		return
	}
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate the context auto-HWM once the
	// target connections are ready, like the C reference
	// (bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp:492).
	perfcommon.Must(serverCtx.RecalculateAutoHwm())

	stats := perfcommon.NewMultiStats()
	window := activeDeadline(cfg.duration)
	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)

	stopRequested := false
	autoHWMPrinted := false
	for !stopRequested {
		wait := time.Until(window.StopAt)
		if wait <= 0 {
			break
		}
		event, pollErr := perfcommon.WaitPollerOne(poller, events, wait)
		if pollErr != nil {
			if perfcommon.IsTransient(pollErr) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi dealer/dealer server poll: %w", pollErr))
		}
		// An interrupted public wait may return no event before StopAt.
		// The active deadline, checked above, owns the measurement boundary.
		if event == nil {
			continue
		}
		if event.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		var received zlink.Received
		for {
			ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if perfcommon.IsTransient(recvErr) {
					break
				}
				perfcommon.Must(fmt.Errorf("multi dealer/dealer server recv: %w", recvErr))
			}
			if !ok {
				break
			}
			parts := received.Parts()
			if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
				stopRequested = true
				_ = received.Close()
				break
			}
			part, partErr := perfcommon.MeasurementPayload(parts)
			if partErr == nil {
				perfcommon.RecordMessageLatency(
					stats, window.ActiveAtNs, window.StopAtNs, cfg.msgSize, part)
				if !autoHWMPrinted {
					// Open the status snapshot only after an application pipe delivered
					// a valid message; an earlier snapshot exposes the raw socket default.
					perfcommon.PrintMultiSocketAutoHWMDetail(
						server, nil, cfg.pattern, cfg.transport, "server", "endpoint", zlink.SocketTypeDealer, cfg.msgSize,
					)
					autoHWMPrinted = true
				}
			}
			_ = received.Close()
		}
	}
	drainMultiDealerDealerActiveTail(server, poller, events, cfg)
	printMultiResult(cfg, stats.Snapshot(cfg.duration, cfg.msgSize))
}

func drainMultiDealerDealerActiveTail(
	server *zlink.DealerSocket,
	poller *zlink.Poller,
	events []zlink.PollEvent,
	cfg multiConfig,
) {
	maxWait := cfg.duration
	if cfg.msgSize >= 65536 {
		maxWait = 2 * cfg.duration
		if maxWait < 10*time.Second {
			maxWait = 10 * time.Second
		}
	} else if maxWait < 2*time.Second {
		maxWait = 2 * time.Second
	}
	deadline := time.Now().Add(maxWait)
	idleWait := 50 * time.Millisecond
	idleDeadline := time.Now().Add(idleWait)
	var received zlink.Received
	for time.Now().Before(deadline) {
		ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
		if recvErr != nil {
			if !perfcommon.IsTransient(recvErr) {
				perfcommon.Must(fmt.Errorf("multi dealer/dealer server tail drain recv: %w", recvErr))
			}
			if !time.Now().Before(idleDeadline) {
				return
			}
			wait := time.Until(idleDeadline)
			if wait <= 0 || wait > idleWait {
				wait = idleWait
			}
			if _, waitErr := perfcommon.WaitPollerOne(poller, events, wait); waitErr != nil && !perfcommon.IsTransient(waitErr) {
				perfcommon.Must(fmt.Errorf("multi dealer/dealer server tail drain poll: %w", waitErr))
			}
			continue
		}
		if !ok {
			if !time.Now().Before(idleDeadline) {
				return
			}
			continue
		}
		idleDeadline = time.Now().Add(idleWait)
		_ = received.Close()
	}
}

func runMultiDealerDealerClient(cfg multiConfig, endpoint string) {
	clients := make([]dealerDealerClient, 0, cfg.clients)
	var sharedCtx *zlink.Context
	if useMultiDealerDealerSharedClientContext(cfg.transport, cfg.msgSize) {
		var err error
		sharedCtx, err = perfcommon.NewMultiClientContext()
		perfcommon.Must(err)
	}
	for i := 0; i < cfg.clients; i++ {
		clientCtx := sharedCtx
		if clientCtx == nil {
			var err error
			clientCtx, err = perfcommon.NewMultiClientContext()
			perfcommon.Must(err)
		}
		client, err := clientCtx.DealerSocket()
		perfcommon.Must(err)
		clientMon := perfcommon.OpenMonitor(client)
		perfcommon.Must(perfcommon.ConfigureTLSClient(client, cfg.transport))
		perfcommon.ApplyMultiHWM(client, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(client, cfg.transport)
		rid := zlink.NewRoutingID([]byte(fmt.Sprintf("dealer-%06d", i)))
		perfcommon.Must(client.SetRoutingID(rid))
		perfcommon.Must(client.Connect(endpoint))
		clients = append(clients, dealerDealerClient{ctx: clientCtx, socket: client, mon: clientMon})
	}
	for _, client := range clients {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), client.mon)
	}
	// PERF_MULTI_TEST_POLICY § 1.6: connection count feeds the plan, so
	// recalculate once every target connection is ready.
	for _, client := range clients {
		perfcommon.Must(client.ctx.RecalculateAutoHwm())
		if sharedCtx != nil {
			break
		}
	}
	defer func() {
		for _, client := range clients {
			_ = client.mon.Close()
			_ = client.socket.Close()
			if sharedCtx == nil {
				_ = client.ctx.Close()
			}
		}
		if sharedCtx != nil {
			_ = sharedCtx.Close()
		}
	}()

	flushControlLine("CLIENT_READY,%d", cfg.msgSize)
	if !waitForStartToken(cfg.msgSize) {
		return
	}
	window := activeDeadline(cfg.duration)
	runMultiDealerDealerSendWindow(clients, cfg, window)
	if len(clients) > 0 {
		perfcommon.PrintMultiSocketAutoHWMDetail(
			clients[0].socket, clients[0].mon, cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeDealer, cfg.msgSize,
		)
	}
	if len(clients) > 0 {
		sendMultiDealerStopToken(clients[0].socket)
	}
	flushControlLine("CLIENT_DONE,%d", cfg.msgSize)
}

func useMultiDealerDealerSharedClientContext(transport string, msgSize int) bool {
	return true
}

func runMultiDealerDealerSendWindow(clients []dealerDealerClient, cfg multiConfig, window perfcommon.BenchmarkWindow) {
	if len(clients) == 0 {
		return
	}

	submit := func(index int) (zlink.SendSubmission, error) {
		client := clients[index]
		var sendErr error
		var submission zlink.SendSubmission
		if perfcommon.MeasurementPartCount() == 2 {
			message := perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt)
			submission, sendErr = perfcommon.SubmitMeasurementSendSubmission(context.Background(), client.socket.Send(), message)
		} else {
			_, sendErr = perfcommon.SubmitRoutedWindowPayload(cfg.msgSize, window.ActiveAt, func(message *zlink.Message) error {
				if !useMultiDealerDealerMoveMessage(cfg.transport, cfg.msgSize) {
					submission, sendErr = client.socket.Send().Message(message).Submit(context.Background())
					return sendErr
				}
				submission, sendErr = client.socket.Send().MoveMessage(message).Submit(context.Background())
				return sendErr
			})
		}
		if sendErr != nil {
			return nil, fmt.Errorf("multi dealer/dealer client send: %w", sendErr)
		}
		return submission, nil
	}
	perfcommon.Must(runMultiSendTurns(
		len(clients), window, "multi dealer/dealer", submit, nil, nil, nil))
}

func useMultiDealerDealerMoveMessage(transport string, msgSize int) bool {
	if msgSize <= 1024 {
		return true
	}
	switch transport {
	case "wss":
		return msgSize >= 65536
	case "tls":
		return msgSize == 131072
	default:
		return false
	}
}

// sendMultiDealerStopToken pushes the wire-level stop token through the
// dealer socket. Submit handles WRITABLE retry internally; the outer bound
// remains for terminal-phase connection and lifecycle failures.
func sendMultiDealerStopToken(socket *zlink.DealerSocket) {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitRoutedPayload(perfcommon.StopToken, func(message *zlink.Message) error {
			return perfcommon.SubmitSend(context.Background(), socket.Send().MoveMessage(message))
		})
		if err == nil && sent {
			return
		}
		if err != nil && !perfcommon.IsTransient(err) {
			return
		}
		perfcommon.PollIdle(perfcommon.StopTokenSendBackoff)
	}
}
