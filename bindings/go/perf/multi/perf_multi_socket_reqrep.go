package main

import (
	"context"
	"fmt"
	"runtime"
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
	}
	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-socket-reqrep")
	flushControlLine("READY,%s", endpoint)
	stop := waitForStopAsync()
	for {
		select {
		case <-stop:
			return
		default:
		}
		var received zlink.Received
		ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
		if recvErr != nil {
			if multiReqRepTransient(recvErr) {
				runtime.Gosched()
				continue
			}
			perfcommon.Must(recvErr)
		}
		if !ok {
			runtime.Gosched()
			continue
		}
		if _, valid := received.ReplyToken(); !valid {
			_ = received.Close()
			perfcommon.Must(fmt.Errorf("multi reqrep request missing reply token"))
		}
		payload, payloadErr := perfcommon.MeasurementPayload(received.Parts())
		perfcommon.Must(payloadErr)
		reply := received.Reply().Message(payload)
		var tail *zlink.Message
		if perfcommon.MeasurementPartCount() == 2 {
			tail = perfcommon.NewMessageWithSize(0)
			reply = reply.Message(tail)
		}
		perfcommon.Must(reply.Submit(context.Background()))
		if tail != nil {
			_ = tail.Close()
		}
		_ = received.Close()
	}
}

func runMultiDealerRouterReqRepClient(cfg multiConfig, endpoint string) perfcommon.Result {
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
	return runMultiReqRepClients(cfg, clients)
}

func runMultiRouterRouterReqRepClient(cfg multiConfig, endpoint string) perfcommon.Result {
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
		perfcommon.Must(socket.SetConnectRoutingID(serverID))
		perfcommon.Must(socket.Connect(endpoint))
		clientSocket := socket
		clients = append(clients, multiReqRepClient{target: socket, monitor: monitor, request: func() zlink.RequestOp { return clientSocket.Request(serverID) }})
	}
	return runMultiReqRepClients(cfg, clients)
}

// The optimized pipelined scheduler is intentionally deferred to Phase 7;
// this sequential loop preserves a compiling, contract-correct benchmark.
func runMultiReqRepClients(cfg multiConfig, clients []multiReqRepClient) perfcommon.Result {
	for i := range clients {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), clients[i].monitor)
		defer clients[i].monitor.Close()
	}
	stats := perfcommon.NewMultiStats()
	window := activeDeadline(cfg.duration)
	timeout := 200 * time.Millisecond
	for time.Now().Before(window.StopAt) {
		for i := range clients {
			payload := perfcommon.PreparePayload(cfg.msgSize)
			perfcommon.StampWindowPayload(payload, window.ActiveAt)
			submit := clients[i].request().Bytes(payload)
			if perfcommon.MeasurementPartCount() == 2 {
				submit = submit.Bytes(nil)
			}
			parts, err := submit.Timeout(timeout).Submit(context.Background())
			if err != nil {
				continue
			}
			reply, payloadErr := perfcommon.MeasurementPayload(parts)
			if payloadErr == nil {
				now := time.Now()
				if latency, valid := perfcommon.LatencyNsFromMessageAt(reply, cfg.msgSize, perfcommon.PhaseActive, now); valid {
					stats.AddCount()
					stats.AddLatencySampleNs(latency / 2.0)
				}
			}
			zlink.MultipartClose(parts)
		}
	}
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func multiReqRepTransient(err error) bool { return err != nil }
