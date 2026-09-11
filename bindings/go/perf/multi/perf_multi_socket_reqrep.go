package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strconv"
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
	autoHWMPrinted := false
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
			if !autoHWMPrinted {
				perfcommon.Must(serverCtx.RecalculateAutoHwm())
				perfcommon.PrintMultiSocketAutoHWMDetail(
					server, nil, cfg.pattern, cfg.transport, "server", "endpoint", zlink.SocketTypeRouter, cfg.msgSize,
				)
				autoHWMPrinted = true
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
		if socketErr != nil {
			perfcommon.Must(fmt.Errorf("multi reqrep create dealer[%d]: %w", i, socketErr))
		}
		monitor := perfcommon.OpenMonitor(socket)
		if err := perfcommon.ConfigureTLSClient(socket, cfg.transport); err != nil {
			perfcommon.Must(fmt.Errorf("multi reqrep configure dealer[%d]: %w", i, err))
		}
		if err := socket.SetRoutingID(zlink.NewRoutingID([]byte(fmt.Sprintf("dealer-req-%06d", i)))); err != nil {
			perfcommon.Must(fmt.Errorf("multi reqrep set dealer routing id[%d]: %w", i, err))
		}
		if err := socket.Connect(endpoint); err != nil {
			perfcommon.Must(fmt.Errorf("multi reqrep connect dealer[%d]: %w", i, err))
		}
		clients = append(clients, multiReqRepClient{target: socket, monitor: monitor, request: socket.Request})
	}
	runMultiReqRepClients(cfg, ctx, clients)
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
	runMultiReqRepClients(cfg, ctx, clients)
}

// Go's public request terminal blocks through reply completion, so each
// logical request runs in its own goroutine while this loop owns completion
// progress for every requester socket.
func runMultiReqRepClients(cfg multiConfig, clientCtx *zlink.Context, clients []multiReqRepClient) {
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
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate after target connections ready.
	if err := clientCtx.RecalculateAutoHwm(); err != nil {
		perfcommon.Must(fmt.Errorf("multi reqrep client auto-HWM recalc: %w", err))
	}
	timeout := 200 * time.Millisecond
	if value, err := strconv.Atoi(os.Getenv("PERF_MULTI_REQREP_TIMEOUT_MS")); err == nil && value > 0 {
		timeout = time.Duration(value) * time.Millisecond
	}
	result := runMultiReqRepWindow(cfg, clients, activeDeadline(cfg.duration), timeout)
	if len(clients) > 0 {
		socketType := zlink.SocketTypeDealer
		if _, ok := clients[0].target.(*zlink.RouterSocket); ok {
			socketType = zlink.SocketTypeRouter
		}
		perfcommon.PrintMultiSocketAutoHWMDetail(
			clients[0].target, clients[0].monitor, cfg.pattern, cfg.transport, "client", "endpoint", socketType, cfg.msgSize,
		)
	}
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
	type admission struct {
		client int
		err    error
	}
	type completion struct {
		client      int
		parts       []*zlink.Message
		err         error
		completedAt time.Time
		completedNs int64
	}
	completed := make(chan completion, len(clients))
	admitted := make(chan admission, len(clients))
	available := make([]bool, len(clients))
	for index := range available {
		available[index] = true
	}
	totalOutstanding := 0
	pendingAdmissions := 0

	submitOne := func(index int) {
		countsByClient[index].attempts++
		payload := perfcommon.PreparePayload(cfg.msgSize)
		perfcommon.StampWindowPayload(payload, window.ActiveAt)
		submit := clients[index].request().Bytes(payload)
		if perfcommon.MeasurementPartCount() == 2 {
			submit = submit.Bytes(nil)
		}
		submission, err := submit.Timeout(timeout).Submit(context.Background())
		if err != nil {
			countsByClient[index].failures++
			perfcommon.Must(err)
		}
		totalOutstanding++
		go func() {
			parts, err := submission.Reply(context.Background())
			completed <- completion{
				client: index, parts: parts, err: err,
				completedAt: time.Now(), completedNs: perfcommon.MonotonicNowNs(),
			}
		}()
		if submission.Result() == zlink.SubmitBackpressured {
			available[index] = false
			pendingAdmissions++
			go func() {
				admitted <- admission{client: index, err: submission.Admitted(context.Background())}
			}()
		}
	}

	processCompletion := func(done completion) {
		totalOutstanding--
		count := &countsByClient[done.client]
		if done.err != nil {
			count.failures++
			var requestErr *zlink.RequestError
			zlink.MultipartClose(done.parts)
			if errors.As(done.err, &requestErr) && requestErr.Result == zlink.RequestTimedOut {
				count.timeouts++
				return
			}
			// BACKPRESSURED with a WRITABLE token is retained and resumed by
			// the binding terminal. Any error returned to the runner is fatal.
			perfcommon.Must(done.err)
		}
		reply, payloadErr := perfcommon.MeasurementPayload(done.parts)
		if payloadErr == nil && done.completedAt.Before(window.StopAt) {
			if latency, valid := perfcommon.LatencyNsFromMessageAt(
				reply, cfg.msgSize, perfcommon.PhaseActive, done.completedNs); valid {
				stats.AddLatencyNs(latency / 2.0)
			}
		}
		zlink.MultipartClose(done.parts)
	}

	drainReady := func() bool {
		progressed := false
		for {
			select {
			case ready := <-admitted:
				pendingAdmissions--
				if ready.err != nil {
					perfcommon.Must(ready.err)
				}
				available[ready.client] = true
				progressed = true
			case done := <-completed:
				processCompletion(done)
				progressed = true
			default:
				return progressed
			}
		}
	}

	waitForProgress := func(deadline time.Time) {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return
		}
		select {
		case ready := <-admitted:
			pendingAdmissions--
			if ready.err != nil {
				perfcommon.Must(ready.err)
			}
			available[ready.client] = true
		case done := <-completed:
			processCompletion(done)
		case <-time.After(remaining):
		}
	}

	for time.Now().Before(window.StopAt) {
		submitted := false
		for index := range clients {
			if !time.Now().Before(window.StopAt) {
				break
			}
			if !available[index] {
				continue
			}
			submitOne(index)
			submitted = true
		}
		progressed := drainReady()
		if !submitted && !progressed {
			waitForProgress(window.StopAt)
		}
	}

	drainTimeout := max(1000*time.Millisecond, timeout*4)
	if value, err := strconv.Atoi(os.Getenv("PERF_MULTI_REQREP_DRAIN_TIMEOUT_MS")); err == nil && value > 0 {
		drainTimeout = time.Duration(value) * time.Millisecond
	}
	drainDeadline := time.Now().Add(drainTimeout)
	for totalOutstanding > 0 || pendingAdmissions > 0 {
		progressed := drainReady()
		if totalOutstanding == 0 && pendingAdmissions == 0 {
			break
		}
		if !time.Now().Before(drainDeadline) {
			perfcommon.Must(fmt.Errorf("multi reqrep completion drain timed out with %d outstanding", totalOutstanding))
		}
		if !progressed {
			waitForProgress(drainDeadline)
		}
	}
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
