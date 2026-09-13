package main

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync/atomic"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type multiRouterClient struct {
	socket  *zlink.RouterSocket
	monitor *zlink.SocketMonitor
}

func runMultiRouterRouterServer(cfg multiConfig) {
	serverCtx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)

	server, err := serverCtx.RouterSocket()
	perfcommon.Must(err)

	teardownDone := make(chan struct{})
	teardown := func() {
		select {
		case <-teardownDone:
			return
		default:
		}
		setRelayStage(relayStageCloseSocket)
		_ = server.Close()
		setRelayStage(relayStageCloseContext)
		_ = serverCtx.Close()
		setRelayStage(relayStageDone)
		close(teardownDone)
	}
	defer teardown()

	serverID := zlink.NewRoutingID([]byte("SERVER"))
	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	perfcommon.Must(server.SetRoutingID(serverID))
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-router-router")
	// PERF_MULTI_TEST_POLICY § 1.6: same recalculation point as the C relay
	// server (bindings/c/perf/multi/common/perf_multi_relay_server.hpp:658).
	perfcommon.Must(serverCtx.RecalculateAutoHwm())
	flushControlLine("READY,%s", endpoint)

	serverDone := make(chan struct{})
	stopSignal := waitForStopAsync()
	watchRelayShutdown(stopSignal, teardownDone)
	go startMultiRouterRouterEchoServer(server, stopSignal, serverDone)
	select {
	case <-serverDone:
	case <-stopSignal:
		// The echo loop observes the same control signal and leaves its poller
		// before the deferred socket/context close runs.
		<-serverDone
	}
}

// Relay teardown stages. The runner kills a server that outlives
// PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS with SIGTERM and then SIGKILL, so the
// stalled stage has to reach stderr while the budget is still running; there
// is no post-mortem output from a killed process.
const (
	relayStageEchoLoop int32 = iota
	relayStageCloseSocket
	relayStageCloseContext
	relayStageDone
)

var relayStageNames = [...]string{"echo_loop", "close_socket", "close_context", "done"}

var (
	relayStage            atomic.Int32
	relayReplySubmitStart atomic.Int64
	relayReplySubmitEnd   atomic.Int64
)

func setRelayStage(stage int32) {
	relayStage.Store(stage)
}

func relayStageLabel() string {
	stage := relayStage.Load()
	if stage < 0 || int(stage) >= len(relayStageNames) {
		return "unknown"
	}
	return relayStageNames[stage]
}

// watchRelayShutdown names the teardown stage on stderr once the control STOP
// arrives, so a server that misses the runner shutdown budget reports which
// step held it and for how long instead of dying silently.
func watchRelayShutdown(stop <-chan struct{}, done <-chan struct{}) {
	go func() {
		select {
		case <-done:
			return
		case <-stop:
		}
		startedNs := perfcommon.MonotonicNowNs()
		ticker := time.NewTicker(500 * time.Millisecond)
		defer ticker.Stop()
		dumped := false
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
			}
			elapsedMs := (perfcommon.MonotonicNowNs() - startedNs) / int64(time.Millisecond)
			started := relayReplySubmitStart.Load()
			finished := relayReplySubmitEnd.Load()
			fmt.Fprintf(
				os.Stderr,
				"[perf-multi-relay] shutdown stalled stage=%s elapsed_ms=%d reply_submit_started=%d reply_submit_finished=%d reply_submit_inflight=%d\n",
				relayStageLabel(), elapsedMs, started, finished, started-finished)
			if !dumped && os.Getenv("PERF_GO_SHUTDOWN_STACK_DUMP") == "1" {
				dumped = true
				buf := make([]byte, 1<<20)
				n := runtime.Stack(buf, true)
				_, _ = os.Stderr.Write(buf[:n])
			}
		}
	}()
}

// relayServerShutdownBudget mirrors the runner contract: STOP on stdin starts
// PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS (run_benchmarks_multi.sh shutdown_server,
// default 5000 ms) after which the server is killed and the case is reported as
// server_shutdown_failed.
func relayServerShutdownBudget() time.Duration {
	for _, name := range []string{
		"PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS",
		"PERF_SERVER_SHUTDOWN_TIMEOUT_MS",
	} {
		if parsed, err := strconv.Atoi(os.Getenv(name)); err == nil && parsed > 0 {
			return time.Duration(parsed) * time.Millisecond
		}
	}
	return 5000 * time.Millisecond
}

// relayShutdownTeardownReserve is the part of the shutdown budget kept for the
// socket and context close that follows the drain.
const relayShutdownTeardownReserve = 2 * time.Second

// relayShutdownDrainWindow bounds the post-STOP reply drain exactly like the C
// relay bounds its own wait token
// (bindings/c/perf/multi/common/perf_multi_relay_server.hpp:524-600): an
// already-admitted reply gets one more chance to complete, but the relay must
// still reach teardown inside the runner shutdown budget.
func relayShutdownDrainWindow() time.Duration {
	window := multiSendDrainTimeout()
	if budget := relayServerShutdownBudget() - relayShutdownTeardownReserve; budget < window {
		window = budget
	}
	if window < 250*time.Millisecond {
		window = 250 * time.Millisecond
	}
	return window
}

func runMultiRouterRouterClientRole(cfg multiConfig, endpoint string) perfcommon.Result {
	serverID := zlink.NewRoutingID([]byte("SERVER"))
	stats := perfcommon.NewMultiStats()
	clientCtx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer clientCtx.Close()

	clients := make([]multiRouterClient, 0, cfg.clients)
	for i := 0; i < cfg.clients; i++ {
		client, socketErr := clientCtx.RouterSocket()
		perfcommon.Must(socketErr)
		clientMon := perfcommon.OpenMonitor(client)
		perfcommon.Must(perfcommon.ConfigureTLSClient(client, cfg.transport))
		perfcommon.ApplyMultiHWM(client, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(client, cfg.transport)
		clientID := zlink.NewRoutingID([]byte(fmt.Sprintf("router-%06d", i)))
		perfcommon.Must(client.SetRoutingID(clientID))
		perfcommon.Must(client.SetConnectRoutingID(serverID))
		perfcommon.Must(client.Connect(endpoint))
		clients = append(clients, multiRouterClient{socket: client, monitor: clientMon})
	}
	for _, client := range clients {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), client.monitor)
	}
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate after target connections ready.
	perfcommon.Must(clientCtx.RecalculateAutoHwm())
	defer func() {
		for _, client := range clients {
			_ = client.monitor.Close()
			_ = client.socket.Close()
		}
	}()

	validateMultiRouterRoutes(serverID, clients, cfg.msgSize)
	window := activeDeadline(cfg.duration)
	runMultiRouterRouterEchoWindow(clients, serverID, cfg, window, stats)
	if len(clients) > 0 {
		perfcommon.PrintMultiSocketAutoHWMDetail(
			clients[0].socket, clients[0].monitor, cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeRouter, cfg.msgSize,
		)
	}
	if len(clients) > 0 {
		sendMultiRouterStopToken(clients[0].socket, serverID)
	}
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func runMultiRouterRouterEchoWindow(
	clients []multiRouterClient,
	serverID zlink.RoutingID,
	cfg multiConfig,
	window perfcommon.BenchmarkWindow,
	stats *perfcommon.Stats,
) {
	if len(clients) == 0 {
		return
	}
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	events := make([]zlink.PollEvent, len(clients))
	for i, client := range clients {
		perfcommon.Must(poller.AddSocket(
			client.socket, perfcommon.ZLinkPollIn|zlink.PollCompletion, uintptr(i)))
	}

	payloads := make([][]byte, len(clients))
	for index := range payloads {
		payloads[index] = perfcommon.PreparePayload(cfg.msgSize)
	}
	submit := func(index int) (zlink.SendSubmission, error) {
		submission, sendErr := sendMultiRouterRouterRequest(clients[index].socket, serverID, payloads[index], window)
		if sendErr != nil {
			return nil, fmt.Errorf("multi router/router send: %w", sendErr)
		}
		return submission, nil
	}
	pendingReplies := 0
	progress := func(wait time.Duration) error {
		n, waitErr := poller.Wait(events, wait)
		if waitErr != nil {
			if perfcommon.IsTransient(waitErr) {
				return nil
			}
			return fmt.Errorf("multi router/router poll: %w", waitErr)
		}
		for i := 0; i < n; i++ {
			idx := int(events[i].Slot)
			if idx < 0 || idx >= len(clients) {
				continue
			}
			socket := clients[idx].socket
			if events[i].Revents&perfcommon.ZLinkPollIn != 0 {
				drained := recvMultiRouterRouterReply(socket, stats, cfg.msgSize, window)
				if drained > pendingReplies {
					return fmt.Errorf("multi router/router received %d replies with %d pending", drained, pendingReplies)
				}
				pendingReplies -= drained
			}
		}
		return nil
	}
	perfcommon.Must(runMultiSendTurns(
		len(clients), window, "multi router/router", submit, progress,
		func(submitted int) { pendingReplies += submitted },
		func() bool { return pendingReplies > 0 }))
}

func sendMultiRouterRouterRequest(
	socket *zlink.RouterSocket,
	serverID zlink.RoutingID,
	payload []byte,
	window perfcommon.BenchmarkWindow,
) (zlink.SendSubmission, error) {
	perfcommon.StampWindowPayload(payload, window.ActiveAt)
	message := perfcommon.NewMessage(payload)
	return perfcommon.SubmitMeasurementSendSubmission(context.Background(), socket.SendTo(serverID), message)
}

func recvMultiRouterRouterReply(
	socket *zlink.RouterSocket,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) int {
	drained, err := drainRouterReplies(
		socket, stats, msgSize, perfcommon.PhaseActive, window.ActiveAtNs, window.StopAtNs)
	if err != nil {
		perfcommon.Must(fmt.Errorf("multi router/router recv: %w", err))
	}
	return drained
}

func validateMultiRouterRoutes(serverID zlink.RoutingID, clients []multiRouterClient, msgSize int) {
	for index, client := range clients {
		poller := perfcommon.NewSocketPoller(
			client.socket, perfcommon.ZLinkPollIn|zlink.PollCompletion)
		events := make([]zlink.PollEvent, 1)
		deadline := time.Now().Add(perfcommon.MultiReadyTimeout())
		validated := false
		probeSubmitted := false
		var pendingProbe zlink.SendSubmission
		for time.Now().Before(deadline) {
			if !probeSubmitted && pendingProbe == nil {
				payload := perfcommon.PreparePayload(msgSize)
				perfcommon.StampProbePayload(payload)
				message := perfcommon.NewMessage(payload)
				submission, sendErr := perfcommon.SubmitMeasurementSendSubmission(
					context.Background(), client.socket.SendTo(serverID), message)
				if sendErr != nil {
					if perfcommon.IsReadyProbeTransient(sendErr) {
						continue
					}
					perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] send: %w", index, sendErr))
				}
				if submission == nil {
					perfcommon.Must(fmt.Errorf(
						"multi router/router route probe[%d] returned no submission", index))
				}
				switch submission.Result() {
				case zlink.SubmitOK:
					probeSubmitted = true
				case zlink.SubmitBackpressured:
					pendingProbe = submission
				default:
					perfcommon.Must(fmt.Errorf(
						"multi router/router route probe[%d] submit result: %d", index, submission.Result()))
				}
			}

			wait := time.Until(deadline)
			if wait > 50*time.Millisecond {
				wait = 50 * time.Millisecond
			}
			event, err := perfcommon.WaitPollerOne(poller, events, wait)
			if err != nil {
				if perfcommon.IsTransient(err) {
					continue
				}
				perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] poll: %w", index, err))
			}
			if pendingProbe != nil {
				ready, admissionErr := multiSendAdmissionReady(pendingProbe)
				if ready {
					if admissionErr != nil {
						perfcommon.Must(fmt.Errorf(
							"multi router/router route probe[%d] admission: %w", index, admissionErr))
					}
					pendingProbe = nil
					probeSubmitted = true
				}
			}
			if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
				continue
			}
			drained, err := drainRouterReplies(
				client.socket, nil, msgSize, perfcommon.PhaseWarmup, 0, 0)
			if err != nil {
				perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] recv: %w", index, err))
			}
			if drained > 0 {
				validated = true
				break
			}
		}
		_ = poller.Close()
		if !validated {
			perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] timed out", index))
		}
	}
}

// startMultiRouterRouterEchoServer runs the echo loop until it receives
// a wire-level stop token from any client. Closes done to notify the
// main goroutine. PERF_MULTI_TEST_POLICY § 1.3.1: poller waits with -1
// (signal-driven) and the loop exits on stop token, not on a stop
// channel.
func startMultiRouterRouterEchoServer(
	server *zlink.RouterSocket,
	stop <-chan struct{},
	done chan<- struct{},
) {
	defer close(done)

	// This receive loop is also the sole completion owner for replies. A
	// backpressured send remains retained while PollCompletion drives its exact
	// WRITABLE retry; no background completion drainer is involved.
	poller := perfcommon.NewSocketPoller(
		server, perfcommon.ZLinkPollIn|zlink.PollCompletion)
	defer poller.Close()
	waitEvents := make([]zlink.PollEvent, 1)

	stopRequested := false
	var shutdownDeadline time.Time
	var pendingReply zlink.SendSubmission

	for {
		select {
		case <-stop:
			stopRequested = true
		default:
		}
		if stopRequested && pendingReply == nil {
			return
		}
		if stopRequested && shutdownDeadline.IsZero() {
			shutdownDeadline = time.Now().Add(relayShutdownDrainWindow())
		}
		if pendingReply != nil && !shutdownDeadline.IsZero() && !time.Now().Before(shutdownDeadline) {
			inflight := relayReplySubmitStart.Load() - relayReplySubmitEnd.Load()
			fmt.Fprintf(
				os.Stderr,
				"[perf-multi-relay] shutdown drain expired window_ms=%d reply_submit_inflight=%d\n",
				relayShutdownDrainWindow().Milliseconds(), inflight)
			relayReplySubmitEnd.Add(1)
			return
		}

		// The data path remains event driven. The bounded wait only lets the
		// role-control signal terminate a quiet server without relying on
		// closing a socket from another goroutine to wake the native poller.
		wait := 100 * time.Millisecond
		if !shutdownDeadline.IsZero() && time.Until(shutdownDeadline) < wait {
			wait = max(time.Until(shutdownDeadline), 0)
		}
		event, err := perfcommon.WaitPollerOne(poller, waitEvents, wait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi router/router server poll: %w", err))
		}
		if pendingReply != nil {
			ready, admissionErr := multiSendAdmissionReady(pendingReply)
			if ready {
				relayReplySubmitEnd.Add(1)
				pendingReply = nil
				if admissionErr != nil && !perfcommon.IsStaleRoute(admissionErr) {
					perfcommon.Must(fmt.Errorf("multi router/router server send: %w", admissionErr))
				}
			}
		}
		if pendingReply != nil || stopRequested || event == nil ||
			event.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		var received zlink.Received
		for pendingReply == nil {
			ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if perfcommon.IsTransient(recvErr) {
					break
				}
				perfcommon.Must(fmt.Errorf("multi router/router server recv: %w", recvErr))
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
			_, partErr := perfcommon.MeasurementPayload(parts)
			if partErr == nil {
				relayReplySubmitStart.Add(1)
				submission, replyErr := submitMultiRouterReply(&received)
				if replyErr != nil {
					relayReplySubmitEnd.Add(1)
					perfcommon.Must(replyErr)
				}
				if submission == nil {
					relayReplySubmitEnd.Add(1)
				} else {
					pendingReply = submission
				}
			}
			_ = received.Close()
		}
	}
}

func submitMultiRouterReply(
	received *zlink.Received,
) (zlink.SendSubmission, error) {
	parts := received.Parts()
	reply := received.Send().MoveMessage(parts[0])
	for _, part := range parts[1:] {
		reply = reply.MoveMessage(part)
	}
	submission, err := reply.Submit(context.Background())
	if err != nil {
		if perfcommon.IsStaleRoute(err) {
			return nil, nil
		}
		return nil, fmt.Errorf("multi router/router server send: %w", err)
	}
	if submission == nil {
		return nil, fmt.Errorf("multi router/router server send returned no submission")
	}
	switch submission.Result() {
	case zlink.SubmitOK:
		return nil, nil
	case zlink.SubmitBackpressured:
		return submission, nil
	default:
		return nil, fmt.Errorf(
			"multi router/router server send returned submit result %d", submission.Result())
	}
}

// sendMultiRouterStopToken pushes the wire-level stop token through the
// supplied router socket addressed to the server. Its public completion poller
// drives WRITABLE retry; the outer bound covers other transient failures.
func sendMultiRouterStopToken(socket *zlink.RouterSocket, serverID zlink.RoutingID) {
	completionPoller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer completionPoller.Close()
	perfcommon.Must(completionPoller.AddSocket(socket, zlink.PollCompletion, 0))
	completionEvents := make([]zlink.PollEvent, 1)
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitRoutedPayload(perfcommon.StopToken, func(message *zlink.Message) error {
			submission, submitErr := socket.SendTo(serverID).MoveMessage(message).Submit(context.Background())
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

func drainRouterReplies(
	socket *zlink.RouterSocket,
	stats *perfcommon.Stats,
	msgSize int,
	phase uint8,
	activeAtNs int64,
	stopAtNs int64,
) (int, error) {
	drained := 0
	var reply zlink.Received
	for {
		ok, err := socket.Recv(&reply, zlink.RecvFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				return drained, nil
			}
			return 0, err
		}
		if !ok {
			return drained, nil
		}
		part, partErr := perfcommon.MeasurementPayload(reply.Parts())
		if partErr == nil && perfcommon.HasMetricHeaderPhase(part.Data(), msgSize, phase) {
			drained++
			if stats != nil {
				perfcommon.RecordMessageRTTLatency(stats, activeAtNs, stopAtNs, msgSize, part)
			}
		}
		_ = reply.Close()
	}
}
