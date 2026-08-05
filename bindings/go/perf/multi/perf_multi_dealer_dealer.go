package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"time"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
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
	perfcommon.ApplyMultiAutoHWMMsgUnit(serverCtx, cfg.msgSize)
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-dealer-dealer")
	flushControlLine("READY,%s", endpoint)
	if !waitForStartToken(cfg.msgSize) {
		return
	}

	stats := perfcommon.NewStats()
	window := activeDeadline(cfg.duration)
	latencyStride := resolveMultiDealerDealerLatencySampleStride(cfg.transport, cfg.msgSize)
	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)

	stopRequested := false
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
		if event == nil {
			break
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
			part, partErr := received.SinglePartOrError()
			if partErr == nil {
				if perfcommon.IsStopTokenMessage(part) {
					stopRequested = true
					_ = received.Close()
					break
				}
				if latencyStride <= 1 {
					now := time.Now()
					if latencyNs, ok := perfcommon.LatencyNsFromMessageAt(part, cfg.msgSize, perfcommon.PhaseActive, now); ok && now.After(window.ActiveAt) && now.Before(window.StopAt) {
						stats.AddLatencyNs(latencyNs)
					}
					_ = received.Close()
					continue
				}
				if perfcommon.HasMetricHeaderPhase(part.Data(), cfg.msgSize, perfcommon.PhaseActive) {
					count := stats.AddCount()
					if shouldSampleMultiDealerDealerLatency(count, latencyStride) {
						now := time.Now()
						if latencyNs, ok := perfcommon.LatencyNsFromMessageAt(part, cfg.msgSize, perfcommon.PhaseActive, now); ok && now.After(window.ActiveAt) && now.Before(window.StopAt) {
							stats.AddLatencySampleNs(latencyNs)
						}
					}
				}
			}
			_ = received.Close()
		}
	}
	drainMultiDealerDealerActiveTail(server, poller, events, cfg)
	printMultiResult(cfg, stats.Snapshot(cfg.duration, cfg.msgSize))
}

func resolveMultiDealerDealerLatencySampleStride(transport string, msgSize int) uint64 {
	if msgSize == 64 {
		return uint64(positiveMultiDealerDealerIntEnv("PERF_MULTI_DEALER_DEALER_LATENCY_SAMPLE_STRIDE", 32))
	}
	return 1
}

func positiveMultiDealerDealerIntEnv(name string, fallback int) int {
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return value
}

func shouldSampleMultiDealerDealerLatency(index, stride uint64) bool {
	return stride <= 1 || index == 1 || index%stride == 0
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
		perfcommon.ApplyMultiAutoHWMMsgUnit(sharedCtx, cfg.msgSize)
	}
	for i := 0; i < cfg.clients; i++ {
		clientCtx := sharedCtx
		if clientCtx == nil {
			var err error
			clientCtx, err = perfcommon.NewMultiClientContext()
			perfcommon.Must(err)
			perfcommon.ApplyMultiAutoHWMMsgUnit(clientCtx, cfg.msgSize)
		}
		client, err := clientCtx.DealerSocket()
		perfcommon.Must(err)
		clientMon := perfcommon.OpenMonitor(client)
		perfcommon.Must(perfcommon.ConfigureTLSClient(client, cfg.transport))
		perfcommon.ApplyMultiHWM(client, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(client, cfg.transport)
		perfcommon.Must(client.Connect(endpoint))
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), clientMon)
		clients = append(clients, dealerDealerClient{ctx: clientCtx, socket: client, mon: clientMon})
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
	// The role goroutine is already pinned to its OS thread by runMultiRole,
	// which keeps the Go scheduler from migrating its M across the blocking
	// poller waits that backpressure produces at high pipe fan-in.
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	events := make([]zlink.PollEvent, len(clients))
	pending := make([]bool, len(clients))
	for i, client := range clients {
		perfcommon.Must(poller.AddSocket(client.socket, 0, uintptr(i)))
	}

	// blast keeps sending on one socket until it backpressures (EAGAIN) or the
	// window ends. On backpressure it registers POLLOUT; on drain it clears it.
	// Mirrors the C++ dealer_dealer client's per-socket try_send_once loop.
	blast := func(i int) {
		client := clients[i]
		for {
			now := time.Now()
			if !now.Before(window.StopAt) {
				break
			}
			sent, sendErr := perfcommon.SubmitWindowPayload(cfg.msgSize, window.ActiveAt, func(message *zlink.Message) (bool, error) {
				if !useMultiDealerDealerMoveMessage(cfg.transport, cfg.msgSize) {
					return client.socket.Send().Message(message).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
				}
				return client.socket.Send().MoveMessage(message).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
			})
			if sendErr == nil && sent {
				if pending[i] {
					pending[i] = false
					perfcommon.Must(poller.ModifySocket(client.socket, 0))
				}
				continue
			}
			if sendErr != nil && !perfcommon.IsTransient(sendErr) {
				perfcommon.Must(fmt.Errorf("multi dealer/dealer client send: %w", sendErr))
			}
			if !pending[i] {
				pending[i] = true
				perfcommon.Must(poller.ModifySocket(client.socket, perfcommon.ZLinkPollOut))
			}
			return
		}
	}

	for time.Now().Before(window.StopAt) {
		pendingCount := 0
		for i := range clients {
			if pending[i] {
				pendingCount++
				continue
			}
			blast(i)
			if pending[i] {
				pendingCount++
			}
		}
		if !time.Now().Before(window.StopAt) || pendingCount == 0 {
			continue
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
			perfcommon.Must(fmt.Errorf("multi dealer/dealer client poll: %w", waitErr))
		}
		// Drain each woken socket inline (C++ parity) instead of clearing the
		// pending flag and waiting for the next full round-robin scan. This
		// keeps the freed pipe busy and avoids an O(N) rescan per wakeup.
		for i := 0; i < n; i++ {
			if events[i].Revents&perfcommon.ZLinkPollOut == 0 {
				continue
			}
			idx := int(events[i].Slot)
			if idx >= 0 && idx < len(pending) && pending[idx] {
				blast(idx)
			}
		}
	}
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
// dealer socket. Bounded attempts through transient backpressure mirror
// the cpp / java / dotnet implementations.
func sendMultiDealerStopToken(socket *zlink.DealerSocket) {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitPayload(perfcommon.StopToken, func(message *zlink.Message) (bool, error) {
			return socket.Send().MoveMessage(message).Submit(context.Background())
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
