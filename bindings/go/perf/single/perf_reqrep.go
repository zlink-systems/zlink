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

// Go exposes a blocking request terminal, so each logical request runs in its
// own goroutine while the requester thread owns completion progress.
func runSingleReqRep(
	cfg benchmarkConfig,
	requester zlink.SocketTarget,
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
	// A single-socket turn submits one request, so only that turn is buffered.
	completed := make(chan completion, 1)
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
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()
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
				stats.AddLatencySampleNs(float64(done.completedNs-sent) / 2.0)
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

	waitForProgress := func(deadline time.Time, progressed bool) {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return
		}
		if progressed {
			remaining = 0
		} else if remaining > 50*time.Millisecond {
			remaining = 50 * time.Millisecond
		}
		_, pollErr := completionPoller.Wait(completionEvents, remaining)
		perfcommon.Must(pollErr)
	}

	// A turn starts one request, drains every completion already available,
	// then lets POLLCOMPLETION pace reply and WRITABLE progress. Outstanding
	// replies never gate submission of the next turn.
	for time.Now().Before(stopAt) {
		submitOne()
		progressed := drainReady()
		waitForProgress(stopAt, progressed)
	}
	for outstanding > 0 {
		progressed := drainReady()
		if outstanding == 0 {
			break
		}
		waitForProgress(time.Now().Add(timeout), progressed)
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
