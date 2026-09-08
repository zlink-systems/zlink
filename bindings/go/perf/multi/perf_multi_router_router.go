package main

import (
	"context"
	"fmt"
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
	defer serverCtx.Close()

	server, err := serverCtx.RouterSocket()
	perfcommon.Must(err)
	defer server.Close()

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
	go startMultiRouterRouterEchoServer(server, stopSignal, serverDone)
	select {
	case <-serverDone:
	case <-stopSignal:
		// The echo loop observes the same control signal and leaves its poller
		// before the deferred socket/context close runs.
		<-serverDone
	}
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
	submit := func(index int) error {
		if sendErr := sendMultiRouterRouterRequest(
			clients[index].socket, serverID, payloads[index], window); sendErr != nil {
			return fmt.Errorf("multi router/router send: %w", sendErr)
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
) error {
	perfcommon.StampWindowPayload(payload, window.ActiveAt)
	message := perfcommon.NewMessage(payload)
	return perfcommon.SubmitMeasurementSend(socket.SendTo(serverID), message)
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
		poller := perfcommon.NewSocketPoller(client.socket, perfcommon.ZLinkPollIn)
		events := make([]zlink.PollEvent, 1)
		deadline := time.Now().Add(perfcommon.MultiReadyTimeout())
		validated := false
		probeSubmitted := false
		for time.Now().Before(deadline) {
			if !probeSubmitted {
				payload := perfcommon.PreparePayload(msgSize)
				perfcommon.StampProbePayload(payload)
				message := perfcommon.NewMessage(payload)
				sendErr := perfcommon.SubmitMeasurementSend(
					client.socket.SendTo(serverID), message)
				if sendErr != nil {
					if perfcommon.IsReadyProbeTransient(sendErr) {
						continue
					}
					perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] send: %w", index, sendErr))
				}
				probeSubmitted = true
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

	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	waitEvents := make([]zlink.PollEvent, 1)

	stopRequested := false

	for !stopRequested {
		select {
		case <-stop:
			return
		default:
		}

		// The data path remains event driven. The bounded wait only lets the
		// role-control signal terminate a quiet server without relying on
		// closing a socket from another goroutine to wake the native poller.
		event, err := perfcommon.WaitPollerOne(poller, waitEvents, 100*time.Millisecond)
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi router/router server poll: %w", err))
		}
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
			part, partErr := perfcommon.MeasurementPayload(parts)
			if partErr == nil {
				routingID := received.RoutingID()
				payload := append([]byte(nil), part.Data()...)
				perfcommon.Must(submitMultiRouterReply(server, routingID, payload))
			}
			_ = received.Close()
		}
	}
}

func submitMultiRouterReply(
	server *zlink.RouterSocket,
	target zlink.RoutingID,
	payload []byte,
) error {
	message := perfcommon.NewMessage(payload)
	err := perfcommon.SubmitMeasurementSend(server.SendTo(target), message)
	if err == nil || perfcommon.IsStaleRoute(err) {
		return nil
	}
	return fmt.Errorf("multi router/router server send: %w", err)
}

// sendMultiRouterStopToken pushes the wire-level stop token through the
// supplied router socket addressed to the server. Submit handles WRITABLE
// retry internally; the outer bound covers other transient shutdown failures.
func sendMultiRouterStopToken(socket *zlink.RouterSocket, serverID zlink.RoutingID) {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitRoutedPayload(perfcommon.StopToken, func(message *zlink.Message) error {
			return socket.SendTo(serverID).MoveMessage(message).Submit(context.Background())
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
