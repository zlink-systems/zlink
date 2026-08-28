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

type pendingRequest struct {
	completion <-chan zlink.RequestReplyCompletion
}

func runSingleReqRep(
	cfg benchmarkConfig,
	requester zlink.SocketTarget,
	replier *zlink.RouterSocket,
	request func() zlink.RequestOp,
	sendStop func(*zlink.Message) (bool, error),
) perfcommon.Result {
	poller := perfcommon.NewSocketPoller(requester, zlink.PollCompletion)
	defer poller.Close()

	localStop := make(chan struct{})
	replierDone := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		replierDone <- runReqRepReplier(replier, localStop)
	}()

	stats, requesterErr := runReqRepRequester(cfg, poller, request)
	stopOK := sendReqRepStop(sendStop)
	if !stopOK {
		close(localStop)
	}
	replierErr := <-replierDone
	if requesterErr != nil || !stopOK || replierErr != nil {
		if os.Getenv("PERF_DEBUG") != "" {
			fmt.Fprintf(os.Stderr, "single reqrep failed requester=%v stop=%t replier=%v\n", requesterErr, stopOK, replierErr)
		}
		if requesterErr != nil {
			perfcommon.Must(requesterErr)
		}
		if !stopOK {
			perfcommon.Must(fmt.Errorf("single reqrep stop token send failed"))
		}
		perfcommon.Must(replierErr)
	}

	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func runReqRepRequester(
	cfg benchmarkConfig,
	poller *zlink.Poller,
	request func() zlink.RequestOp,
) (*perfcommon.Stats, error) {
	stats := perfcommon.NewStats()
	pending := make([]pendingRequest, 0)
	events := make([]zlink.PollEvent, 1)
	requestTimeout := reqRepDurationFromEnv("PERF_SINGLE_REQREP_TIMEOUT_MS", 200*time.Millisecond)
	drainTimeout := reqRepDurationFromEnv("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10*time.Second)
	activeAt := time.Now()
	stopAt := activeAt.Add(cfg.duration)

	// Keep submission and public completion progress in this requester loop.
	// PollCompletion drains replies without a progress goroutine, timer, pipe,
	// or sleep fallback.
	for time.Now().Before(stopAt) {
		for time.Now().Before(stopAt) {
			completion, submitErr := submitReqRepMeasurement(request(), cfg.msgSize, activeAt, requestTimeout)
			if submitErr != nil {
				if isReqRepTransient(submitErr) {
					break
				}
				return stats, submitErr
			}
			ready, submitBlocked, err := consumeReqRepCompletion(completion, stats, cfg.msgSize, stopAt)
			if err != nil {
				return stats, err
			}
			if ready {
				if submitBlocked {
					break
				}
			} else {
				pending = append(pending, pendingRequest{completion: completion})
			}
		}
		if len(pending) == 0 {
			continue
		}
		var err error
		pending, _, err = pollReqRepCompletions(
			poller, events, pending, stats, cfg.msgSize, stopAt, 50*time.Millisecond)
		if err != nil {
			return stats, err
		}
	}

	// The active deadline closes submission. Only requests admitted before that
	// boundary remain eligible during the bounded completion drain.
	drainDeadline := time.Now().Add(drainTimeout)
	for len(pending) > 0 && time.Now().Before(drainDeadline) {
		wait := 50 * time.Millisecond
		if remaining := time.Until(drainDeadline); remaining < wait {
			wait = remaining
		}
		var err error
		pending, _, err = pollReqRepCompletions(
			poller, events, pending, stats, cfg.msgSize, stopAt, wait)
		if err != nil {
			return stats, err
		}
	}
	if len(pending) != 0 {
		return stats, fmt.Errorf("single reqrep completion drain timed out with %d requests", len(pending))
	}
	if !stats.Snapshot(cfg.duration, cfg.msgSize).Valid {
		return stats, fmt.Errorf("single reqrep completed no valid requests")
	}
	return stats, nil
}

func submitReqRepMeasurement(
	request zlink.RequestOp,
	msgSize int,
	activeAt time.Time,
	timeout time.Duration,
) (<-chan zlink.RequestReplyCompletion, error) {
	payload := perfcommon.NewWindowMessage(msgSize, activeAt)
	submit := request.Message(payload)
	var tail *zlink.Message
	if perfcommon.MeasurementPartCount() == 2 {
		tail = perfcommon.NewMessageWithSize(0)
		submit = submit.Message(tail)
	}
	completion, err := submit.Timeout(timeout).
		Flags(zlink.SendFlagsDontWait).
		Submit(context.Background())
	_ = payload.Close()
	if tail != nil {
		_ = tail.Close()
	}
	return completion, err
}

func pollReqRepCompletions(
	poller *zlink.Poller,
	events []zlink.PollEvent,
	pending []pendingRequest,
	stats *perfcommon.Stats,
	msgSize int,
	activeStopAt time.Time,
	timeout time.Duration,
) ([]pendingRequest, bool, error) {
	if _, err := poller.Wait(events, timeout); err != nil && !isReqRepTransient(err) {
		return pending, false, err
	}
	remaining := pending[:0]
	blocked := false
	for _, item := range pending {
		ready, itemBlocked, err := consumeReqRepCompletion(item.completion, stats, msgSize, activeStopAt)
		if err != nil {
			return remaining, blocked, err
		}
		if !ready {
			remaining = append(remaining, item)
		}
		blocked = blocked || itemBlocked
	}
	return remaining, blocked, nil
}

func consumeReqRepCompletion(
	completion <-chan zlink.RequestReplyCompletion,
	stats *perfcommon.Stats,
	msgSize int,
	activeStopAt time.Time,
) (bool, bool, error) {
	select {
	case result, ok := <-completion:
		if !ok {
			return true, false, fmt.Errorf("single reqrep completion closed without a result")
		}
		defer zlink.MultipartClose(result.Parts)
		if result.Err != nil {
			var submitErr *zlink.SubmitError
			if errors.As(result.Err, &submitErr) && isReqRepTransient(result.Err) {
				return true, true, nil
			}
			if result.Result == zlink.RequestTimedOut {
				return true, false, nil
			}
			return true, false, result.Err
		}
		if result.Result != zlink.RequestOK {
			return true, false, fmt.Errorf("single reqrep completion result: %d", result.Result)
		}
		payload, err := perfcommon.MeasurementPayload(result.Parts)
		if err != nil {
			return true, false, err
		}
		now := time.Now()
		if !now.Before(activeStopAt) {
			return true, false, nil
		}
		sentTsNs, valid := perfcommon.SentTimestampNsFromMessagePhase(
			payload, msgSize, perfcommon.PhaseActive)
		if valid {
			stats.AddCount()
			if nowNs := now.UnixNano(); nowNs >= sentTsNs {
				stats.AddLatencySampleNs(float64(nowNs - sentTsNs))
			}
		}
		return true, false, nil
	default:
		return false, false, nil
	}
}

func runReqRepReplier(replier *zlink.RouterSocket, localStop <-chan struct{}) error {
	drainTimeout := reqRepDurationFromEnv("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10*time.Second)
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
			if isReqRepTransient(err) {
				continue
			}
			return err
		}
		if !ok {
			continue
		}
		if !received.HasRoutingID() {
			return fmt.Errorf("single reqrep request has no source routing id")
		}
		parts := received.Parts()
		if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
			return nil
		}
		if !received.HasRequestSeq() {
			return fmt.Errorf("single reqrep request has no request sequence")
		}
		payload, err := perfcommon.MeasurementPayload(parts)
		if err != nil {
			return err
		}
		deadline := time.Now().Add(drainTimeout)
		for {
			submit := received.Reply().Message(payload)
			var tail *zlink.Message
			if perfcommon.MeasurementPartCount() == 2 {
				tail = perfcommon.NewMessageWithSize(0)
				submit = submit.Message(tail)
			}
			err = submit.Submit(context.Background())
			if tail != nil {
				_ = tail.Close()
			}
			if err == nil {
				break
			}
			if !isReqRepTransient(err) || !time.Now().Before(deadline) {
				return err
			}
			runtime.Gosched()
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
		if err != nil && !isReqRepTransient(err) {
			return false
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

func isReqRepTransient(err error) bool {
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
