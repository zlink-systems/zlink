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
	requester zlink.SocketTarget,
	requesterMonitor *zlink.SocketMonitor,
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

	status, err := requesterMonitor.Status()
	perfcommon.Must(err)
	hwmBytes := status.AutoHwmAppliedSndHwmBytes
	if hwmBytes == 0 {
		options, ok := requester.(interface {
			CommonOptions() *zlink.CommonSocketOptions
		})
		if !ok {
			perfcommon.Must(fmt.Errorf("single reqrep requester has no common socket options"))
		}
		hwmBytes, err = options.CommonOptions().SendHighWaterMark()
		perfcommon.Must(err)
	}
	if hwmBytes == 0 {
		perfcommon.Must(fmt.Errorf("single reqrep requester reports no send HWM"))
	}
	wireSize := max(cfg.msgSize, perfcommon.MetricHeaderSize)
	windowBytes := hwmBytes / uint64(wireSize)
	if windowBytes == 0 {
		windowBytes = 1
	}
	maxInt := uint64(^uint(0) >> 1)
	if windowBytes > maxInt {
		windowBytes = maxInt
	}
	admissionWindow := int(windowBytes)

	completionPoller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer completionPoller.Close()
	// PollCompletion transfers the binding's completion-drain ownership to this
	// poller. The requester goroutine that submits requests also calls Wait.
	perfcommon.Must(completionPoller.AddSocket(requester, zlink.PollCompletion, 0))
	completionEvents := make([]zlink.PollEvent, 1)
	pending := make([]zlink.RequestSubmission, 0, admissionWindow)
	// Reply first checks whether its completion is already published. A canceled
	// context therefore makes the same-goroutine pending scan nonblocking.
	readyContext, cancelReady := context.WithCancel(context.Background())
	cancelReady()

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
		if submission.Result() != zlink.SubmitOK && submission.Result() != zlink.SubmitBackpressured {
			perfcommon.Must(fmt.Errorf("single reqrep request returned submit result %d", submission.Result()))
		}
		pending = append(pending, submission)
	}

	processCompletion := func(parts []*zlink.Message, completionErr error, completedAt time.Time, completedNs int64) {
		if completionErr != nil {
			zlink.MultipartClose(parts)
			var requestErr *zlink.RequestError
			if errors.As(completionErr, &requestErr) && requestErr.Result == zlink.RequestTimedOut {
				return
			}
			perfcommon.Must(completionErr)
		}
		reply, payloadErr := perfcommon.MeasurementPayload(parts)
		if payloadErr == nil && completedAt.Before(stopAt) {
			if sent, valid := perfcommon.SentTimestampNsFromMessagePhase(reply, cfg.msgSize, perfcommon.PhaseActive); valid && completedNs >= sent {
				stats.AddCount()
				// PERF_SINGLE_TEST_POLICY.md 1.1.4: request-reply latency is the
				// whole round trip, from the request submission stamp to this
				// reply completion. The C reference records the same interval
				// undivided (bindings/c/perf/single/common/perf_single_reqrep.hpp
				// request_completion_callback).
				stats.AddLatencySampleNs(float64(completedNs - sent))
			}
		}
		zlink.MultipartClose(parts)
	}

	settleReady := func() {
		keep := 0
		for _, submission := range pending {
			parts, replyErr := submission.Reply(readyContext)
			if errors.Is(replyErr, context.Canceled) {
				pending[keep] = submission
				keep++
				continue
			}
			processCompletion(parts, replyErr, time.Now(), perfcommon.MonotonicNowNs())
		}
		pending = pending[:keep]
	}

	progressCompletions := func(wait time.Duration) {
		ready, waitErr := completionPoller.Wait(completionEvents, wait)
		perfcommon.Must(waitErr)
		if ready > 0 {
			settleReady()
		}
	}

	for time.Now().Before(stopAt) {
		submittedSinceProgress := 0
		for time.Now().Before(stopAt) && len(pending) < admissionWindow {
			submitOne()
			submittedSinceProgress++
			if submittedSinceProgress >= 64 {
				submittedSinceProgress = 0
				progressCompletions(0)
			}
		}
		wait := 50 * time.Millisecond
		if remaining := time.Until(stopAt); remaining < wait {
			wait = max(remaining, 0)
		}
		progressCompletions(wait)
	}
	for len(pending) > 0 {
		progressCompletions(50 * time.Millisecond)
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
