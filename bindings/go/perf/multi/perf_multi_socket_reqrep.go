package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"syscall"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type multiReqRepClient struct {
	target     zlink.SocketTarget
	monitor    *zlink.SocketMonitor
	request    func() zlink.RequestOp
	completion []<-chan zlink.RequestReplyCompletion
}

func runMultiDealerRouterReqRepServer(cfg multiConfig) {
	runMultiSocketReqRepServer(cfg, false)
}

func runMultiRouterRouterReqRepServer(cfg multiConfig) {
	runMultiSocketReqRepServer(cfg, true)
}

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
	poller := perfcommon.NewSocketPoller(server, zlink.PollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)
	for {
		select {
		case <-stop:
			return
		default:
		}
		event, waitErr := perfcommon.WaitPollerOne(poller, events, 100*time.Millisecond)
		if waitErr != nil {
			if multiReqRepTransient(waitErr) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi reqrep server poll: %w", waitErr))
		}
		if event == nil || event.Revents&zlink.PollIn == 0 {
			continue
		}
		for {
			var received zlink.Received
			ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if multiReqRepTransient(recvErr) {
					break
				}
				perfcommon.Must(fmt.Errorf("multi reqrep server recv: %w", recvErr))
			}
			if !ok {
				break
			}
			if !received.HasRoutingID() || !received.HasRequestSeq() {
				_ = received.Close()
				perfcommon.Must(fmt.Errorf("multi reqrep request missing reply route"))
			}
			payload, payloadErr := perfcommon.MeasurementPayload(received.Parts())
			if payloadErr != nil {
				_ = received.Close()
				perfcommon.Must(payloadErr)
			}
			deadline := time.Now().Add(multiReqRepDrainTimeout())
			for {
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
				if replyErr == nil {
					break
				}
				if !multiReqRepTransient(replyErr) || !time.Now().Before(deadline) {
					_ = received.Close()
					perfcommon.Must(fmt.Errorf("multi reqrep server reply: %w", replyErr))
				}
				runtime.Gosched()
			}
			_ = received.Close()
		}
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
		perfcommon.ApplyMultiHWM(socket, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(socket, cfg.transport)
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
		perfcommon.ApplyMultiHWM(socket, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(socket, cfg.transport)
		perfcommon.Must(socket.SetRoutingID(zlink.NewRoutingID([]byte(fmt.Sprintf("router-req-%06d", i)))))
		perfcommon.Must(socket.SetConnectRoutingID(serverID))
		perfcommon.Must(socket.Connect(endpoint))
		clientSocket := socket
		clients = append(clients, multiReqRepClient{
			target:  socket,
			monitor: monitor,
			request: func() zlink.RequestOp { return clientSocket.Request(serverID) },
		})
	}
	return runMultiReqRepClients(cfg, clients)
}

func runMultiReqRepClients(cfg multiConfig, clients []multiReqRepClient) perfcommon.Result {
	for i := range clients {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), clients[i].monitor)
	}
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

	stats := perfcommon.NewMultiStats()
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	events := make([]zlink.PollEvent, len(clients))
	for i := range clients {
		perfcommon.Must(poller.AddSocket(clients[i].target, zlink.PollCompletion, uintptr(i)))
	}

	window := activeDeadline(cfg.duration)
	requestTimeout := multiReqRepDurationFromEnv("PERF_MULTI_REQREP_TIMEOUT_MS", 200*time.Millisecond)
	for time.Now().Before(window.StopAt) {
		for i := range clients {
			for time.Now().Before(window.StopAt) {
				completion, submitErr := submitMultiReqRep(clients[i].request(), cfg.msgSize, window.ActiveAt, requestTimeout)
				if submitErr != nil {
					if multiReqRepTransient(submitErr) {
						break
					}
					perfcommon.Must(fmt.Errorf("multi reqrep submit: %w", submitErr))
				}
				clients[i].completion = append(clients[i].completion, completion)
			}
		}
		if !time.Now().Before(window.StopAt) {
			break
		}
		wait := time.Until(window.StopAt)
		if wait > 50*time.Millisecond {
			wait = 50 * time.Millisecond
		}
		if _, waitErr := poller.Wait(events, wait); waitErr != nil && !multiReqRepTransient(waitErr) {
			perfcommon.Must(fmt.Errorf("multi reqrep completion poll: %w", waitErr))
		}
		drainMultiReqRepCompletions(clients, stats, cfg.msgSize, window)
	}

	drainDeadline := time.Now().Add(multiReqRepDrainTimeout())
	for multiReqRepPending(clients) && time.Now().Before(drainDeadline) {
		wait := time.Until(drainDeadline)
		if wait > 50*time.Millisecond {
			wait = 50 * time.Millisecond
		}
		if _, waitErr := poller.Wait(events, wait); waitErr != nil && !multiReqRepTransient(waitErr) {
			perfcommon.Must(fmt.Errorf("multi reqrep bounded drain poll: %w", waitErr))
		}
		drainMultiReqRepCompletions(clients, stats, cfg.msgSize, window)
	}
	if multiReqRepPending(clients) {
		perfcommon.Must(fmt.Errorf("multi reqrep completion drain timed out"))
	}
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func submitMultiReqRep(request zlink.RequestOp, msgSize int, activeAt time.Time, timeout time.Duration) (<-chan zlink.RequestReplyCompletion, error) {
	payload := perfcommon.NewWindowMessage(msgSize, activeAt)
	submit := request.Message(payload)
	var tail *zlink.Message
	if perfcommon.MeasurementPartCount() == 2 {
		tail = perfcommon.NewMessageWithSize(0)
		submit = submit.Message(tail)
	}
	completion, err := submit.Timeout(timeout).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
	_ = payload.Close()
	if tail != nil {
		_ = tail.Close()
	}
	return completion, err
}

func drainMultiReqRepCompletions(clients []multiReqRepClient, stats *perfcommon.Stats, msgSize int, window perfcommon.BenchmarkWindow) {
	for i := range clients {
		remaining := clients[i].completion[:0]
		for _, completion := range clients[i].completion {
			select {
			case result, ok := <-completion:
				if !ok {
					perfcommon.Must(fmt.Errorf("multi reqrep completion closed without result"))
				}
				if result.Err == nil && result.Result == zlink.RequestOK {
					payload, payloadErr := perfcommon.MeasurementPayload(result.Parts)
					if payloadErr == nil {
						perfcommon.RecordMessageRTTLatency(stats, window.ActiveAt, window.StopAt, msgSize, payload)
					}
				}
				zlink.MultipartClose(result.Parts)
			default:
				remaining = append(remaining, completion)
			}
		}
		clients[i].completion = remaining
	}
}

func multiReqRepPending(clients []multiReqRepClient) bool {
	for i := range clients {
		if len(clients[i].completion) != 0 {
			return true
		}
	}
	return false
}

func multiReqRepDrainTimeout() time.Duration {
	requestTimeout := multiReqRepDurationFromEnv("PERF_MULTI_REQREP_TIMEOUT_MS", 200*time.Millisecond)
	fallback := 4 * requestTimeout
	if fallback < time.Second {
		fallback = time.Second
	}
	return multiReqRepDurationFromEnv("PERF_MULTI_REQREP_DRAIN_TIMEOUT_MS", fallback)
}

func multiReqRepDurationFromEnv(name string, fallback time.Duration) time.Duration {
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return time.Duration(value) * time.Millisecond
}

func multiReqRepTransient(err error) bool {
	var zerr zlink.ZlinkError
	if !errors.As(err, &zerr) {
		return false
	}
	switch zerr.InternalErrno() {
	case int(syscall.EAGAIN), int(syscall.EINTR), int(syscall.ETIMEDOUT),
		int(syscall.EHOSTUNREACH), int(syscall.ENOTCONN):
		return true
	default:
		return false
	}
}
