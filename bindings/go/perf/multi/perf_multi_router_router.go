package main

import (
	"context"
	"fmt"
	"time"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

type multiRouterClient struct {
	socket  *zlink.RouterSocket
	monitor *zlink.SocketMonitor
}

type pendingRouterReply struct {
	routingID zlink.RoutingID
	payload   []byte
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
	perfcommon.ApplyMultiAutoHWMMsgUnit(serverCtx, cfg.msgSize)
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	perfcommon.Must(server.SetRoutingID(serverID))
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-router-router")
	flushControlLine("READY,%s", endpoint)

	serverDone := make(chan struct{})
	go startMultiRouterRouterEchoServer(server, serverDone)
	select {
	case <-serverDone:
	case <-waitForStopAsync():
	}
}

func runMultiRouterRouterClientRole(cfg multiConfig, endpoint string) perfcommon.Result {
	serverID := zlink.NewRoutingID([]byte("SERVER"))
	stats := perfcommon.NewStats()
	clientCtx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer clientCtx.Close()
	perfcommon.ApplyMultiAutoHWMMsgUnit(clientCtx, cfg.msgSize)

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
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), clientMon)
		clients = append(clients, multiRouterClient{socket: client, monitor: clientMon})
	}
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
	payloads := make([][]byte, len(clients))
	waitingReply := make([]bool, len(clients))
	sendPending := make([]bool, len(clients))
	for i, client := range clients {
		perfcommon.Must(poller.AddSocket(client.socket, perfcommon.ZLinkPollOut, uintptr(i)))
		payloads[i] = perfcommon.PreparePayload(cfg.msgSize)
	}

	for time.Now().Before(window.StopAt) {
		for i := range clients {
			if waitingReply[i] || sendPending[i] {
				continue
			}
			if sendMultiRouterRouterRequest(clients[i].socket, serverID, payloads[i], cfg.msgSize, window) {
				waitingReply[i] = true
				perfcommon.Must(poller.ModifySocket(clients[i].socket, perfcommon.ZLinkPollIn))
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
			perfcommon.Must(fmt.Errorf("multi router/router poll: %w", waitErr))
		}
		for i := 0; i < n; i++ {
			idx := int(events[i].Slot)
			if idx < 0 || idx >= len(clients) {
				continue
			}
			socket := clients[idx].socket
			if events[i].Revents&perfcommon.ZLinkPollIn != 0 && waitingReply[idx] {
				if recvMultiRouterRouterReply(socket, stats, cfg.msgSize, window) {
					waitingReply[idx] = false
					perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollOut))
				}
			}
			if events[i].Revents&perfcommon.ZLinkPollOut != 0 && sendPending[idx] {
				if sendMultiRouterRouterRequest(socket, serverID, payloads[idx], cfg.msgSize, window) {
					sendPending[idx] = false
					waitingReply[idx] = true
					perfcommon.Must(poller.ModifySocket(socket, perfcommon.ZLinkPollIn))
				}
			}
		}
	}
}

func sendMultiRouterRouterRequest(
	socket *zlink.RouterSocket,
	serverID zlink.RoutingID,
	payload []byte,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) bool {
	perfcommon.StampWindowPayload(payload, window.ActiveAt)
	sent, err := tryRouterSend(socket, serverID, payload)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false
		}
		perfcommon.Must(fmt.Errorf("multi router/router send: %w", err))
	}
	return sent
}

func recvMultiRouterRouterReply(
	socket *zlink.RouterSocket,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
) bool {
	drained, err := drainRouterReplies(socket, stats, msgSize, window.ActiveAt, window.StopAt)
	if err != nil {
		perfcommon.Must(fmt.Errorf("multi router/router recv: %w", err))
	}
	return drained
}

func validateMultiRouterRoutes(serverID zlink.RoutingID, clients []multiRouterClient, msgSize int) {
	for index, client := range clients {
		payload := perfcommon.PreparePayload(msgSize)
		perfcommon.StampProbePayload(payload)
		_, sendErr := perfcommon.SubmitPayload(payload, func(message *zlink.Message) (bool, error) {
			return client.socket.SendTo(serverID).MoveMessage(message).Submit(context.Background())
		})
		perfcommon.Must(sendErr)

		poller := perfcommon.NewSocketPoller(client.socket, perfcommon.ZLinkPollIn)
		events := make([]zlink.PollEvent, 1)
		deadline := time.Now().Add(perfcommon.MultiReadyTimeout())
		validated := false
		for time.Now().Before(deadline) {
			event, err := perfcommon.WaitPollerOne(poller, events, time.Until(deadline))
			if err != nil {
				if perfcommon.IsTransient(err) {
					continue
				}
				perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] poll: %w", index, err))
			}
			if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
				continue
			}
			drained, err := drainRouterReplies(client.socket, nil, msgSize, time.Time{}, time.Time{})
			if err != nil {
				perfcommon.Must(fmt.Errorf("multi router/router route probe[%d] recv: %w", index, err))
			}
			if drained {
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
func startMultiRouterRouterEchoServer(server *zlink.RouterSocket, done chan<- struct{}) {
	defer close(done)

	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	waitEvents := make([]zlink.PollEvent, 1)

	pending := make([]pendingRouterReply, 0, 8)
	stopRequested := false
	activeEvents := perfcommon.ZLinkPollIn

	for !stopRequested {
		events := perfcommon.ZLinkPollIn
		if len(pending) > 0 {
			events |= perfcommon.ZLinkPollOut
		}
		if events != activeEvents {
			if err := poller.ModifySocket(server, events); err != nil {
				perfcommon.Must(fmt.Errorf("multi router/router server modify: %w", err))
			}
			activeEvents = events
		}

		event, err := perfcommon.WaitPollerOne(poller, waitEvents, -1*time.Millisecond)
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi router/router server poll: %w", err))
		}
		if event == nil {
			continue
		}

		if event.Revents&perfcommon.ZLinkPollOut != 0 {
			for len(pending) > 0 {
				sent, sendErr := tryRouterSend(server, pending[0].routingID, pending[0].payload)
				if sendErr != nil {
					perfcommon.Must(fmt.Errorf("multi router/router server send: %w", sendErr))
				}
				if !sent {
					break
				}
				pending = pending[1:]
			}
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

			part, partErr := received.SinglePartOrError()
			if partErr == nil {
				if perfcommon.IsStopTokenMessage(part) {
					stopRequested = true
					_ = received.Close()
					break
				}
				routingID := received.RoutingID()
				payload := part.Data()
				if len(pending) == 0 {
					sent, sendErr := tryRouterSend(server, routingID, payload)
					if sendErr != nil {
						_ = received.Close()
						perfcommon.Must(fmt.Errorf("multi router/router server send: %w", sendErr))
					}
					if sent {
						_ = received.Close()
						continue
					}
				}
				pending = append(pending, pendingRouterReply{
					routingID: routingID,
					payload:   append([]byte(nil), payload...),
				})
			}
			_ = received.Close()
		}
	}
}

// sendMultiRouterStopToken pushes the wire-level stop token through the
// supplied router socket addressed to the server. Bounded attempts through
// transient backpressure.
func sendMultiRouterStopToken(socket *zlink.RouterSocket, serverID zlink.RoutingID) {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitPayload(perfcommon.StopToken, func(message *zlink.Message) (bool, error) {
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
	activeAt time.Time,
	stopAt time.Time,
) (bool, error) {
	drained := false
	var reply zlink.Received
	for {
		ok, err := socket.Recv(&reply, zlink.RecvFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				return drained, nil
			}
			return false, err
		}
		if !ok {
			return drained, nil
		}
		part, partErr := reply.SinglePartOrError()
		if partErr == nil && stats != nil {
			perfcommon.RecordMessageRTTLatency(stats, activeAt, stopAt, msgSize, part)
		}
		drained = true
		_ = reply.Close()
	}
}

func tryRouterSend(socket *zlink.RouterSocket, target zlink.RoutingID, payload []byte) (bool, error) {
	return perfcommon.SubmitPayload(payload, func(message *zlink.Message) (bool, error) {
		return socket.SendTo(target).MoveMessage(message).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
	})
}
