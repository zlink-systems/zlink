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
	target  zlink.SocketTarget
	monitor *zlink.SocketMonitor
	request func() zlink.RequestOp
}

type multiReqRepSubmit func([]byte, time.Duration) (<-chan zlink.RequestReplyCompletion, error)

type multiReqRepEventKind uint8

const (
	multiReqRepAdmitted multiReqRepEventKind = iota
	multiReqRepCompleted
	multiReqRepAdmissionStopped
	multiReqRepCompletionStopped
	multiReqRepFailed
)

type multiReqRepEvent struct {
	kind            multiReqRepEventKind
	client          int
	result          zlink.RequestReplyCompletion
	err             error
	beforeAdmission bool
	completedAt     time.Time
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
				if replyErr == nil || perfcommon.IsStaleRoute(replyErr) {
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
	submitters := make([]multiReqRepSubmit, len(clients))
	for i := range clients {
		request := clients[i].request
		submitters[i] = func(payload []byte, timeout time.Duration) (<-chan zlink.RequestReplyCompletion, error) {
			return submitMultiReqRep(request(), payload, timeout)
		}
	}

	window := activeDeadline(cfg.duration)
	requestTimeout := multiReqRepDurationFromEnv("PERF_MULTI_REQREP_TIMEOUT_MS", 200*time.Millisecond)
	perfcommon.Must(runMultiReqRepScheduler(
		submitters, stats, cfg.msgSize, window, requestTimeout, multiReqRepDrainTimeout()))
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func runMultiReqRepScheduler(
	submitters []multiReqRepSubmit,
	stats *perfcommon.Stats,
	msgSize int,
	window perfcommon.BenchmarkWindow,
	requestTimeout time.Duration,
	drainTimeout time.Duration,
) error {
	if len(submitters) == 0 || !time.Now().Before(window.StopAt) {
		return nil
	}

	events := make(chan multiReqRepEvent)
	done := make(chan struct{})
	defer close(done)
	stopAdmissions := make(chan struct{})
	admissionsStopped := false
	stopAdmissionWorkers := func() {
		if !admissionsStopped {
			close(stopAdmissions)
			admissionsStopped = true
		}
	}
	admissionWorkers := len(submitters)
	completionWorkers := len(submitters)
	running := 0
	for i := range submitters {
		completions := make(chan (<-chan zlink.RequestReplyCompletion))
		go runMultiReqRepAdmissionWorker(
			i, submitters[i], msgSize, requestTimeout, window, completions,
			events, stopAdmissions, done)
		go runMultiReqRepCompletionMux(i, completions, events, done)
	}

	var firstErr error
	handle := func(event multiReqRepEvent) {
		switch event.kind {
		case multiReqRepAdmitted:
			running++
		case multiReqRepCompleted:
			running--
			if event.result.Err == nil && event.result.Result == zlink.RequestOK {
				payload, payloadErr := perfcommon.MeasurementPayload(event.result.Parts)
				if payloadErr != nil {
					if firstErr == nil {
						firstErr = payloadErr
					}
				} else if !event.completedAt.Before(window.ActiveAt) &&
					event.completedAt.Before(window.StopAt) {
					latencyNs, ok := perfcommon.LatencyNsFromMessageAt(
						payload, msgSize, perfcommon.PhaseActive, event.completedAt)
					if ok {
						stats.AddCount()
						stats.AddLatencySampleNs(latencyNs / 2.0)
					}
				}
			}
			zlink.MultipartClose(event.result.Parts)
		case multiReqRepAdmissionStopped:
			admissionWorkers--
		case multiReqRepCompletionStopped:
			completionWorkers--
		case multiReqRepFailed:
			if event.beforeAdmission {
				admissionWorkers--
			} else {
				running--
			}
			if firstErr == nil {
				firstErr = event.err
			}
		}
	}

	activeTimer := time.NewTimer(time.Until(window.StopAt))
	active := true
	for active && firstErr == nil {
		select {
		case event := <-events:
			handle(event)
		case <-activeTimer.C:
			active = false
		}
		if !time.Now().Before(window.StopAt) {
			active = false
		}
	}
	stopAdmissionWorkers()
	if !activeTimer.Stop() {
		select {
		case <-activeTimer.C:
		default:
		}
	}

	drainTimer := time.NewTimer(drainTimeout)
	defer drainTimer.Stop()
	for admissionWorkers > 0 || completionWorkers > 0 || running > 0 {
		select {
		case event := <-events:
			handle(event)
		case <-drainTimer.C:
			return fmt.Errorf(
				"multi reqrep bounded drain timed out with %d requests, %d admission workers, %d completion workers",
				running, admissionWorkers, completionWorkers)
		}
	}
	return firstErr
}

func runMultiReqRepAdmissionWorker(
	client int,
	submit multiReqRepSubmit,
	msgSize int,
	timeout time.Duration,
	window perfcommon.BenchmarkWindow,
	completions chan<- (<-chan zlink.RequestReplyCompletion),
	events chan<- multiReqRepEvent,
	stopAdmissions <-chan struct{},
	done <-chan struct{},
) {
	defer close(completions)
	for multiReqRepAdmissionActive(window.StopAt, stopAdmissions) {
		payload := perfcommon.PreparePayload(msgSize)
		perfcommon.StampWindowPayload(payload, window.ActiveAt)
		for {
			completion, err := submit(payload, timeout)
			if err != nil {
				if multiReqRepPreAdmission(err) {
					if !multiReqRepAdmissionActive(window.StopAt, stopAdmissions) {
						multiReqRepSendEvent(events, done, multiReqRepEvent{
							kind: multiReqRepAdmissionStopped, client: client,
						})
						return
					}
					runtime.Gosched()
					continue
				}
				multiReqRepSendEvent(events, done, multiReqRepEvent{
					kind: multiReqRepFailed, client: client, err: err, beforeAdmission: true,
				})
				return
			}
			if completion == nil {
				multiReqRepSendEvent(events, done, multiReqRepEvent{
					kind: multiReqRepFailed, client: client,
					err:             fmt.Errorf("multi reqrep admission returned a nil completion"),
					beforeAdmission: true,
				})
				return
			}

			if !multiReqRepSendEvent(events, done, multiReqRepEvent{
				kind: multiReqRepAdmitted, client: client,
			}) {
				return
			}
			select {
			case completions <- completion:
			case <-done:
				return
			}
			break
		}
	}
	multiReqRepSendEvent(events, done, multiReqRepEvent{
		kind: multiReqRepAdmissionStopped, client: client,
	})
}

func runMultiReqRepCompletionMux(
	client int,
	completions <-chan (<-chan zlink.RequestReplyCompletion),
	events chan<- multiReqRepEvent,
	done <-chan struct{},
) {
	pending := make([]<-chan zlink.RequestReplyCompletion, 0)
	admissionsOpen := true
	for admissionsOpen || len(pending) > 0 {
		var next <-chan zlink.RequestReplyCompletion
		if len(pending) > 0 {
			next = pending[0]
		}
		select {
		case completion, ok := <-completions:
			if !ok {
				admissionsOpen = false
				completions = nil
				continue
			}
			pending = append(pending, completion)
		case <-done:
			return
		case result, ok := <-next:
			pending[0] = nil
			pending = pending[1:]
			if !ok {
				if !multiReqRepSendEvent(events, done, multiReqRepEvent{
					kind: multiReqRepFailed, client: client,
					err: fmt.Errorf("multi reqrep completion closed without result"),
				}) {
					return
				}
				continue
			}
			if !multiReqRepSendEvent(events, done, multiReqRepEvent{
				kind: multiReqRepCompleted, client: client,
				result: result, completedAt: time.Now(),
			}) {
				zlink.MultipartClose(result.Parts)
				return
			}
		}
	}
	multiReqRepSendEvent(events, done, multiReqRepEvent{
		kind: multiReqRepCompletionStopped, client: client,
	})
}

func multiReqRepAdmissionActive(stopAt time.Time, stop <-chan struct{}) bool {
	if !time.Now().Before(stopAt) {
		return false
	}
	select {
	case <-stop:
		return false
	default:
		return true
	}
}

func multiReqRepSendEvent(
	events chan<- multiReqRepEvent,
	done <-chan struct{},
	event multiReqRepEvent,
) bool {
	select {
	case events <- event:
		return true
	case <-done:
		return false
	}
}

func submitMultiReqRep(
	request zlink.RequestOp,
	payload []byte,
	timeout time.Duration,
) (<-chan zlink.RequestReplyCompletion, error) {
	submit := request.Bytes(payload)
	if perfcommon.MeasurementPartCount() == 2 {
		submit = submit.Bytes(nil)
	}
	return submit.Timeout(timeout).
		Flags(zlink.SendFlagsNone).
		Submit(context.Background())
}

func multiReqRepPreAdmission(err error) bool {
	var submitErr *zlink.SubmitError
	if !errors.As(err, &submitErr) {
		return false
	}
	return submitErr.Result == zlink.SubmitBackpressured ||
		submitErr.Result == zlink.SubmitNotAdmitted ||
		submitErr.InternalErrno() == int(syscall.EINTR)
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
