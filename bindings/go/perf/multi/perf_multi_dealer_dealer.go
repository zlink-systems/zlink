package main

import (
	"context"
	"fmt"
	"sync"
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

	stats := perfcommon.NewMultiStats()
	window := activeDeadline(cfg.duration)
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
			parts := received.Parts()
			if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
				stopRequested = true
				_ = received.Close()
				break
			}
			part, partErr := perfcommon.MeasurementPayload(parts)
			if partErr == nil {
				perfcommon.RecordMessageLatency(
					stats, window.ActiveAt, window.StopAt, cfg.msgSize, part)
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
	// Go has no separate async send terminal. Each long-lived goroutine owns
	// one socket's blocking Submit call; the Go scheduler keeps the remaining
	// sockets progressing while Core suspends a backpressured submit.
	var workers sync.WaitGroup
	errors := make(chan error, len(clients))
	for _, client := range clients {
		client := client
		workers.Add(1)
		go func() {
			defer workers.Done()
			for time.Now().Before(window.StopAt) {
				var sendErr error
				if perfcommon.MeasurementPartCount() == 2 {
					message := perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt)
					sendErr = perfcommon.SubmitMeasurementRouted(client.socket.Send(), message)
				} else {
					_, sendErr = perfcommon.SubmitRoutedWindowPayload(cfg.msgSize, window.ActiveAt, func(message *zlink.Message) error {
						if !useMultiDealerDealerMoveMessage(cfg.transport, cfg.msgSize) {
							return client.socket.Send().Message(message).Submit(context.Background())
						}
						return client.socket.Send().MoveMessage(message).Submit(context.Background())
					})
				}
				if sendErr == nil || perfcommon.IsTransient(sendErr) {
					continue
				}
				select {
				case errors <- fmt.Errorf("multi dealer/dealer client send: %w", sendErr):
				default:
				}
				return
			}
		}()
	}
	workers.Wait()
	select {
	case err := <-errors:
		perfcommon.Must(err)
	default:
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
		sent, err := perfcommon.SubmitRoutedPayload(perfcommon.StopToken, func(message *zlink.Message) error {
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
