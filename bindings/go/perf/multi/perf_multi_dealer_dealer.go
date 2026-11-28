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

	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)
	receive := func(window perfcommon.BenchmarkWindow, paced bool) (perfcommon.Result, uint64) {
		stats := perfcommon.NewMultiStats()
		end := window.StopAt.Add(multiSendDrainTimeout())
		remaining := cfg.clients
		var pacedReceived uint64
		for remaining > 0 && time.Now().Before(end) {
			event, err := perfcommon.WaitPollerOne(poller, events, multiWait(time.Now(), end))
			if err != nil && !perfcommon.IsTransient(err) {
				perfcommon.Must(err)
			}
			if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
				continue
			}
			var received zlink.Received
			for remaining > 0 && time.Now().Before(end) {
				ok, err := server.Recv(&received, zlink.RecvFlagsDontWait)
				if err != nil && !perfcommon.IsTransient(err) {
					perfcommon.Must(err)
				}
				if !ok || err != nil {
					break
				}
				now := perfcommon.MonotonicNowNs()
				parts := received.Parts()
				if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
					remaining--
				} else if part, err := perfcommon.MeasurementPayload(parts); err == nil {
					if recordMultiReceive(stats, part.Data(), cfg.msgSize, now, window, paced, 1) {
						pacedReceived++
					}
				}
				_ = received.Close()
			}
		}
		if remaining != 0 {
			perfcommon.Must(fmt.Errorf("multi dealer/dealer receive drain timed out"))
		}
		return stats.Snapshot(cfg.duration, cfg.msgSize), pacedReceived
	}
	active, _ := receive(activeDeadline(cfg.duration), false)
	if active.Count == 0 {
		printMultiResult(cfg, active)
		return
	}
	flushControlLine("PHASE_DRAINED,%d,%d", cfg.msgSize, active.Count)
	paced, pacedReceived := receive(activeDeadline(settle+multiLatencyDuration(cfg.duration)), true)
	flushControlLine("PHASE_COMPLETE,%d,%d", cfg.msgSize, pacedReceived)
	perfcommon.PrintMultiSocketAutoHWMDetail(server, nil, cfg.pattern, cfg.transport, "server", "endpoint", zlink.SocketTypeDealer, cfg.msgSize)
	paced.Throughput, paced.Bandwidth = active.Throughput, active.Bandwidth
	printMultiResult(cfg, paced)
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
	active := runMultiDealerDealerSendWindow(clients, cfg, activeDeadline(cfg.duration), nil)
	for _, client := range clients {
		sendMultiDealerStopToken(client.socket)
	}
	received := readPhaseCount("PHASE_PACED", cfg.msgSize)
	window := activeDeadline(settle + multiLatencyDuration(cfg.duration))
	schedule := newMultiSchedule(window.ActiveAt, received, cfg.duration)
	runMultiDealerDealerSendWindow(clients, cfg, window, schedule)
	for _, client := range clients {
		sendMultiDealerStopToken(client.socket)
	}
	paced := readPhaseCount("PHASE_COMPLETE", cfg.msgSize)
	schedule.print(cfg, active, received, paced, false, -1)
	if len(clients) > 0 {
		perfcommon.PrintMultiSocketAutoHWMDetail(
			clients[0].socket, clients[0].mon, cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeDealer, cfg.msgSize,
		)
	}
	flushControlLine("CLIENT_DONE,%d", cfg.msgSize)
}

func useMultiDealerDealerSharedClientContext(transport string, msgSize int) bool {
	return true
}

func runMultiDealerDealerSendWindow(clients []dealerDealerClient, cfg multiConfig, window perfcommon.BenchmarkWindow, schedule *multiSchedule) uint64 {
	completionPoller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer completionPoller.Close()
	completionEvents := make([]zlink.PollEvent, len(clients))
	for index := range clients {
		perfcommon.Must(completionPoller.AddSocket(clients[index].socket, zlink.PollCompletion, uintptr(index)))
	}
	submit := func(index int) (zlink.SendSubmission, error) {
		phase := perfcommon.PhaseActive
		if schedule != nil {
			phase = schedule.phase(time.Now())
		}
		message := perfcommon.NewMessageWithSize(cfg.msgSize)
		perfcommon.StampPayloadPhase(message.Data(), phase)
		op := clients[index].socket.Send()
		if perfcommon.MeasurementPartCount() == 2 {
			return perfcommon.SubmitMeasurementSendSubmission(context.Background(), op, message)
		}
		defer message.Close()
		if useMultiDealerDealerMoveMessage(cfg.transport, cfg.msgSize) {
			return op.MoveMessage(message).Submit(context.Background())
		}
		return op.Message(message).Submit(context.Background())
	}
	progress := func(wait time.Duration, _ func() error) error {
		_, err := completionPoller.Wait(completionEvents, wait)
		if perfcommon.IsTransient(err) {
			return nil
		}
		return err
	}
	sent, err := runMultiSendTurns(len(clients), window, schedule, cfg.pattern, submit, progress, nil, nil)
	perfcommon.Must(err)
	return sent
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
// dealer socket. Its public completion poller drives a WRITABLE retry before
// the outer bound handles other terminal-phase connection/lifecycle failures.
func sendMultiDealerStopToken(socket *zlink.DealerSocket) {
	completionPoller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer completionPoller.Close()
	perfcommon.Must(completionPoller.AddSocket(socket, zlink.PollCompletion, 0))
	completionEvents := make([]zlink.PollEvent, 1)
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitRoutedPayload(perfcommon.StopToken, func(message *zlink.Message) error {
			submission, submitErr := socket.Send().MoveMessage(message).Submit(context.Background())
			if submitErr != nil {
				return submitErr
			}
			return waitMultiSendAdmission(completionPoller, completionEvents, submission)
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
