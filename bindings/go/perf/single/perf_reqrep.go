package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

func runSingleReqRep(
	cfg benchmarkConfig,
	_ zlink.SocketTarget,
	_ *zlink.SocketMonitor,
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
	completed := make(chan completion)
	outstanding := 0

	submitOne := func() {
		payload := perfcommon.NewWindowMessage(cfg.msgSize, activeAt)
		submit := request().Message(payload)
		var tail *zlink.Message
		if perfcommon.MeasurementPartCount() == 2 {
			tail = perfcommon.NewMessageWithSize(0)
			submit = submit.Message(tail)
		}
		submission, err := submit.Timeout(timeout).Submit(context.Background())
		_ = payload.Close()
		if tail != nil {
			_ = tail.Close()
		}
		perfcommon.Must(err)
		outstanding++
		go func() {
			parts, err := submission.Reply(context.Background())
			completedAt := time.Now()
			completedNs := perfcommon.MonotonicNowNs()
			completed <- completion{parts: parts, err: err, completedAt: completedAt, completedNs: completedNs}
		}()
		if submission.Result() == zlink.SubmitBackpressured {
			perfcommon.Must(submission.Admitted(context.Background()))
		}
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

	for time.Now().Before(stopAt) {
		submitOne()
		drainReady()
	}
	for outstanding > 0 {
		processCompletion(<-completed)
	}

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
