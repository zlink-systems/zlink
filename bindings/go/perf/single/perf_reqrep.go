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

// Go exposes a blocking request terminal, so each capped requester thread owns
// both its admission and reply completion instead of serializing the role at 1 RTT.
func runSingleReqRep(
	cfg benchmarkConfig,
	requester zlink.SocketTarget,
	replier *zlink.RouterSocket,
	request func() zlink.RequestOp,
	sendStop func(*zlink.Message) (bool, error),
) perfcommon.Result {
	_ = requester
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
	maxOutstanding := 64
	if raw := os.Getenv("PERF_SINGLE_REQREP_MAX_OUTSTANDING"); raw != "" {
		if value, err := strconv.Atoi(raw); err == nil && value > 0 {
			maxOutstanding = value
		}
	}
	if maxOutstanding < 2 {
		maxOutstanding = 2
	}
	var requesters sync.WaitGroup
	requesters.Add(maxOutstanding)
	for range maxOutstanding {
		go func() {
			defer requesters.Done()
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()
			for time.Now().Before(stopAt) {
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
				if err != nil {
					zlink.MultipartClose(parts)
					var requestErr *zlink.RequestError
					if errors.As(err, &requestErr) && requestErr.Result == zlink.RequestTimedOut {
						continue
					}
					perfcommon.Must(err)
				}
				reply, payloadErr := perfcommon.MeasurementPayload(parts)
				if payloadErr == nil && completedAt.Before(stopAt) {
					if sent, valid := perfcommon.SentTimestampNsFromMessagePhase(reply, cfg.msgSize, perfcommon.PhaseActive); valid && completedNs >= sent {
						stats.AddCount()
						stats.AddLatencySampleNs(float64(completedNs-sent) / 2.0)
					}
				}
				zlink.MultipartClose(parts)
			}
		}()
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
