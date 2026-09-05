package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type multiReqRepClient struct {
	target  zlink.SocketTarget
	monitor *zlink.SocketMonitor
	request func() zlink.RequestOp
}

func runMultiDealerRouterReqRepServer(cfg multiConfig) { runMultiSocketReqRepServer(cfg, false) }
func runMultiRouterRouterReqRepServer(cfg multiConfig) { runMultiSocketReqRepServer(cfg, true) }

func runMultiSocketReqRepServer(cfg multiConfig, hasRoutingID bool) {
	serverCtx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)
	defer serverCtx.Close()
	server, err := serverCtx.RouterSocket()
	perfcommon.Must(err)
	defer server.Close()
	if hasRoutingID {
		perfcommon.Must(server.SetRoutingID(zlink.NewRoutingID([]byte("SERVER"))))
		perfcommon.Must(server.SetMandatory(true))
	}
	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-socket-reqrep")
	flushControlLine("READY,%s", endpoint)
	stop := waitForStopAsync()
	poller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)
	for {
		select {
		case <-stop:
			return
		default:
		}
		event, pollErr := perfcommon.WaitPollerOne(poller, events, 100*time.Millisecond)
		if pollErr != nil {
			if perfcommon.IsTransient(pollErr) {
				continue
			}
			perfcommon.Must(pollErr)
		}
		if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		var received zlink.Received
		for {
			select {
			case <-stop:
				return
			default:
			}
			ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if perfcommon.IsTransient(recvErr) {
					break
				}
				perfcommon.Must(recvErr)
			}
			if !ok {
				break
			}
			payload, payloadErr := perfcommon.MeasurementPayload(received.Parts())
			perfcommon.Must(payloadErr)
			if _, valid := received.ReplyToken(); !valid || !received.HasRoutingID() {
				_ = received.Close()
				perfcommon.Must(fmt.Errorf("multi reqrep request missing routing ID or reply token"))
			}
			reply := received.Reply().Message(payload)
			var tail *zlink.Message
			if perfcommon.MeasurementPartCount() == 2 {
				tail = perfcommon.NewMessageWithSize(0)
				reply = reply.Message(tail)
			}
			replyErr := reply.Submit(context.Background())
			if tail != nil {
				_ = tail.Close()
			}
			_ = received.Close()
			if replyErr != nil {
				select {
				case <-stop:
					return
				default:
					perfcommon.Must(replyErr)
				}
			}
		}
	}
}

func runMultiDealerRouterReqRepClient(cfg multiConfig, endpoint string) {
	ctx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer ctx.Close()
	clients := make([]multiReqRepClient, 0, cfg.clients)
	for i := 0; i < cfg.clients; i++ {
		socket, socketErr := ctx.DealerSocket()
		perfcommon.Must(socketErr)
		monitor := perfcommon.OpenMonitor(socket)
		perfcommon.Must(perfcommon.ConfigureTLSClient(socket, cfg.transport))
		perfcommon.Must(socket.SetRoutingID(zlink.NewRoutingID([]byte(fmt.Sprintf("dealer-req-%06d", i)))))
		perfcommon.Must(socket.Connect(endpoint))
		clients = append(clients, multiReqRepClient{target: socket, monitor: monitor, request: socket.Request})
	}
	runMultiReqRepClients(cfg, clients)
}

func runMultiRouterRouterReqRepClient(cfg multiConfig, endpoint string) {
	ctx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer ctx.Close()
	serverID := zlink.NewRoutingID([]byte("SERVER"))
	clients := make([]multiReqRepClient, 0, cfg.clients)
	for i := 0; i < cfg.clients; i++ {
		socket, socketErr := ctx.RouterSocket()
		perfcommon.Must(socketErr)
		monitor := perfcommon.OpenMonitor(socket)
		perfcommon.Must(perfcommon.ConfigureTLSClient(socket, cfg.transport))
		perfcommon.Must(socket.SetRoutingID(zlink.NewRoutingID([]byte(fmt.Sprintf("router-req-%06d", i)))))
		perfcommon.Must(socket.SetMandatory(true))
		perfcommon.Must(socket.SetConnectRoutingID(serverID))
		perfcommon.Must(socket.Connect(endpoint))
		clientSocket := socket
		clients = append(clients, multiReqRepClient{
			target:  socket,
			monitor: monitor,
			request: func() zlink.RequestOp { return clientSocket.Request(serverID) },
		})
	}
	runMultiReqRepClients(cfg, clients)
}

// Each socket has one goroutine owning its blocking request terminal. The Go
// runtime suspends that goroutine while the other sockets continue submitting.
func runMultiReqRepClients(cfg multiConfig, clients []multiReqRepClient) {
	defer func() {
		for i := range clients {
			_ = clients[i].monitor.Close()
			switch socket := clients[i].target.(type) {
			case *zlink.DealerSocket:
				_ = socket.Close()
			case *zlink.RouterSocket:
				_ = socket.Close()
			}
		}
	}()
	for i := range clients {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), clients[i].monitor)
	}
	timeout := 200 * time.Millisecond
	if value, err := strconv.Atoi(os.Getenv("PERF_MULTI_REQREP_TIMEOUT_MS")); err == nil && value > 0 {
		timeout = time.Duration(value) * time.Millisecond
	}
	result := runMultiReqRepWindow(cfg, clients, activeDeadline(cfg.duration), timeout)
	printMultiResult(cfg, result)
	flushControlLine("CLIENT_DONE,%d", cfg.msgSize)
	// The server can still be finishing a reply. Keep its completion targets
	// alive until the runner has stopped the server, then close the sockets.
	waitForStopToken()
}

func runMultiReqRepWindow(cfg multiConfig, clients []multiReqRepClient, window perfcommon.BenchmarkWindow, timeout time.Duration) perfcommon.Result {
	stats := perfcommon.NewMultiStats()
	type counts struct{ attempts, failures, timeouts uint64 }
	countsByClient := make([]counts, len(clients))
	var workers sync.WaitGroup
	for i := range clients {
		workers.Add(1)
		go func(i int) {
			defer workers.Done()
			count := &countsByClient[i]
			for time.Now().Before(window.StopAt) {
				payload := perfcommon.PreparePayload(cfg.msgSize)
				perfcommon.StampWindowPayload(payload, window.ActiveAt)
				submit := clients[i].request().Bytes(payload)
				if perfcommon.MeasurementPartCount() == 2 {
					submit = submit.Bytes(nil)
				}
				count.attempts++
				parts, err := submit.Timeout(timeout).Submit(context.Background())
				if err != nil {
					count.failures++
					var requestErr *zlink.RequestError
					zlink.MultipartClose(parts)
					// Failed admissions are fatal, as in the C submit loop.
					// Native request completions (including timeout) count as
					// failed round trips and leave this socket ready to submit.
					if !errors.As(err, &requestErr) {
						perfcommon.Must(err)
					}
					if requestErr.Result == zlink.RequestTimedOut {
						count.timeouts++
					}
					continue
				}
				reply, payloadErr := perfcommon.MeasurementPayload(parts)
				if payloadErr == nil {
					now := time.Now()
					if now.Before(window.StopAt) {
						if latency, valid := perfcommon.LatencyNsFromMessageAt(reply, cfg.msgSize, perfcommon.PhaseActive, now); valid {
							stats.AddLatencyNs(latency / 2.0)
						}
					}
				}
				zlink.MultipartClose(parts)
			}
		}(i)
	}
	// Finish submitted terminals before socket teardown, but exclude replies
	// received after the active deadline, as the C completion callback does.
	workers.Wait()
	var total counts
	for _, count := range countsByClient {
		total.attempts += count.attempts
		total.failures += count.failures
		total.timeouts += count.timeouts
	}
	fmt.Fprintf(os.Stderr, "REQREP_COUNTS,%s,%s,%d,attempts=%d,failures=%d,timeouts=%d\n",
		cfg.pattern, cfg.transport, cfg.msgSize, total.attempts, total.failures, total.timeouts)
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}
