package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

// PERF_SINGLE_TEST_POLICY.md 1.1.3 (D-BP40): the blocking request terminal
// never reports admission, so the runner reproduces the C reference boundary
// from the admission window Core actually applied to this socket - the applied
// SNDHWM bytes divided by one request's wire size. That is the same window
// whose exhaustion makes the C runner see ZLINK_SUBMIT_BACKPRESSURED; it is not
// a fixed number. A manual PERF_SINGLE_SNDHWM override does not reach the
// auto-HWM snapshot, so it is read back from the socket option instead.
func reqRepAdmissionWindow(
	requester zlink.SocketTarget,
	monitor *zlink.SocketMonitor,
	wireSize int,
) int {
	var hwmBytes uint64
	if monitor != nil {
		if snapshot, err := monitor.Status(); err == nil && snapshot != nil {
			hwmBytes = snapshot.AutoHwmAppliedSndHwmBytes
		}
	}
	if hwmBytes == 0 {
		if getter, ok := requester.(interface {
			SendHighWaterMark() (uint64, error)
		}); ok {
			if value, err := getter.SendHighWaterMark(); err == nil {
				hwmBytes = value
			}
		}
	}
	if hwmBytes == 0 {
		perfcommon.Must(fmt.Errorf(
			"requester socket reports no send high-water mark, so the admission window is unknown"))
	}
	if wireSize < 1 {
		wireSize = 1
	}
	window := int(hwmBytes / uint64(wireSize))
	// One in-flight request is always allowed so a window narrower than a
	// single message still makes progress.
	if window < 1 {
		window = 1
	}
	return window
}

// Go exposes a blocking request terminal, so each logical request runs in its
// own goroutine while the requester thread owns completion progress.
func runSingleReqRep(
	cfg benchmarkConfig,
	requester zlink.SocketTarget,
	requesterMon *zlink.SocketMonitor,
	replier *zlink.RouterSocket,
	request func() zlink.RequestOp,
	sendStop func(*zlink.Message) (bool, error),
) perfcommon.Result {
	localStop := make(chan struct{})
	replierDone := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		replierDone <- runReqRepReplier(replier, localStop)
	}()

	admissionWindow := reqRepAdmissionWindow(requester, requesterMon, cfg.msgSize)

	stats := perfcommon.NewStats()
	activeAt := time.Now()
	stopAt := activeAt.Add(cfg.duration)
	timeout := reqRepDurationFromEnv("PERF_SINGLE_REQREP_TIMEOUT_MS", 200*time.Millisecond)
	type completion struct {
		parts       []*zlink.Message
		err         error
		completedAt time.Time
		completedNs int64
	}
	// Every un-settled request can report at most once, so the admission window
	// also sizes this hand-off. A submitting goroutine never blocks on it and
	// therefore never holds a slot the requester thread believes is free.
	completed := make(chan completion, admissionWindow)
	outstanding := 0
	completionPoller := perfcommon.NewSocketPoller(requester, zlink.PollCompletion)
	defer completionPoller.Close()
	completionEvents := make([]zlink.PollEvent, 1)
	var requesters sync.WaitGroup

	submitOne := func() {
		outstanding++
		requesters.Add(1)
		go func() {
			defer requesters.Done()
			// No runtime.LockOSThread here. These are not the role goroutines
			// of PERF_SINGLE_TEST_POLICY.md 1.1.4 - the requester role is the
			// locked benchmark goroutine that owns the completion poller, and
			// the replier role is the locked goroutine above. A per-request
			// goroutine only performs one DONTWAIT admission and then parks on
			// the binding's completion channel; locking it would wire one OS
			// thread per un-settled request and the admission window is
			// thousands of requests wide at small message sizes.
			payload := perfcommon.NewWindowMessage(cfg.msgSize, activeAt)
			submit := request().Message(payload)
			var tail *zlink.Message
			if perfcommon.MeasurementPartCount() == 2 {
				tail = perfcommon.NewMessageWithSize(0)
				submit = submit.Message(tail)
			}
			parts, err := submit.Timeout(timeout).Submit(context.Background())
			completedAt := time.Now()
			completedNs := perfcommon.MonotonicNowNs()
			_ = payload.Close()
			if tail != nil {
				_ = tail.Close()
			}
			completed <- completion{parts: parts, err: err, completedAt: completedAt, completedNs: completedNs}
		}()
	}

	processCompletion := func(done completion) {
		outstanding--
		if done.err != nil {
			zlink.MultipartClose(done.parts)
			var requestErr *zlink.RequestError
			if errors.As(done.err, &requestErr) && requestErr.Result == zlink.RequestTimedOut {
				return
			}
			perfcommon.Must(done.err)
		}
		reply, payloadErr := perfcommon.MeasurementPayload(done.parts)
		if payloadErr == nil && done.completedAt.Before(stopAt) {
			if sent, valid := perfcommon.SentTimestampNsFromMessagePhase(reply, cfg.msgSize, perfcommon.PhaseActive); valid && done.completedNs >= sent {
				stats.AddCount()
				// PERF_SINGLE_TEST_POLICY.md 1.1.4: request-reply latency is the
				// whole round trip, from the request submission stamp to this
				// reply completion. The C reference records the same interval
				// undivided (bindings/c/perf/single/common/perf_single_reqrep.hpp
				// request_completion_callback).
				stats.AddLatencySampleNs(float64(done.completedNs - sent))
			}
		}
		zlink.MultipartClose(done.parts)
	}

	drainReady := func() bool {
		progressed := false
		for {
			select {
			case done := <-completed:
				processCompletion(done)
				progressed = true
			default:
				return progressed
			}
		}
	}

	// The requester thread owns the only completion drain for this socket, so
	// POLLCOMPLETION progress has to run here. A zero timeout is C's poll(0)
	// after a submission burst; the bounded one is C's poll(50) once the
	// admission window is saturated.
	progressOnce := func(wait time.Duration) {
		if wait < 0 {
			wait = 0
		}
		_, pollErr := completionPoller.Wait(completionEvents, wait)
		perfcommon.Must(pollErr)
	}

	// The requester thread owns the completion drain, so a settled request is
	// only visible here after its goroutine has run. Yielding once turns a
	// scheduling gap into a drained completion instead of a blocking poll.
	// This is a scheduler yield, not a wait: it never sleeps.
	drainSettled := func() bool {
		if drainReady() {
			return true
		}
		if outstanding == 0 {
			return false
		}
		runtime.Gosched()
		return drainReady()
	}

	// C parity (perf_single_reqrep.hpp run_request_phase): one turn submits
	// continuously without awaiting any reply until the applied admission
	// window is full, drains completions without waiting every 64 submissions,
	// and blocks bounded only once the window is saturated.
	for time.Now().Before(stopAt) {
		submittedSinceProgress := 0
		for time.Now().Before(stopAt) && outstanding < admissionWindow {
			submitOne()
			submittedSinceProgress++
			if submittedSinceProgress >= 64 {
				submittedSinceProgress = 0
				progressOnce(0)
				drainSettled()
			}
		}
		// The window is full (or the deadline passed): progress on this thread
		// and block bounded so a saturated interval cannot spin.
		if !drainSettled() {
			progressOnce(50 * time.Millisecond)
			drainSettled()
		}
	}
	// Completion drain of the requests submitted before the deadline. Every one
	// of them is bounded by its own Timeout(timeout), so this terminates; none
	// of them is counted (processCompletion drops completions past stopAt).
	for outstanding > 0 {
		if !drainSettled() {
			progressOnce(50 * time.Millisecond)
			drainSettled()
		}
	}
	requesters.Wait()

	if !sendReqRepStop(sendStop) {
		close(localStop)
	}
	perfcommon.Must(<-replierDone)
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func runReqRepReplier(replier *zlink.RouterSocket, localStop <-chan struct{}) error {
	var received zlink.Received
	defer received.Close()
	for {
		select {
		case <-localStop:
			return nil
		default:
		}
		ok, err := replier.Recv(&received, zlink.RecvFlagsNone)
		if err != nil {
			return err
		}
		if !ok {
			continue
		}
		parts := received.Parts()
		if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
			return nil
		}
		if _, valid := received.ReplyToken(); !valid {
			return fmt.Errorf("single reqrep request has no reply token")
		}
		payload, err := perfcommon.MeasurementPayload(parts)
		if err != nil {
			return err
		}
		reply := received.Reply().Message(payload)
		var tail *zlink.Message
		if perfcommon.MeasurementPartCount() == 2 {
			tail = perfcommon.NewMessageWithSize(0)
			reply = reply.Message(tail)
		}
		err = reply.Submit(context.Background())
		if tail != nil {
			_ = tail.Close()
		}
		if err != nil {
			return err
		}
		_ = received.Close()
	}
}

func sendReqRepStop(send func(*zlink.Message) (bool, error)) bool {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		message := perfcommon.NewMessage(perfcommon.StopToken)
		sent, err := send(message)
		_ = message.Close()
		if err == nil && sent {
			return true
		}
		perfcommon.PollIdle(perfcommon.StopTokenSendBackoff)
	}
	return false
}

func reqRepDurationFromEnv(name string, fallback time.Duration) time.Duration {
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
