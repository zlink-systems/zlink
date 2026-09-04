package main

import (
	"context"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

// Request Submit owns any pre-admission WRITABLE retry and then waits for the
// normal reply completion, so the benchmark loop needs no pending-request FIFO.
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
	for time.Now().Before(stopAt) {
		payload := perfcommon.NewWindowMessage(cfg.msgSize, activeAt)
		submit := request().Message(payload)
		var tail *zlink.Message
		if perfcommon.MeasurementPartCount() == 2 {
			tail = perfcommon.NewMessageWithSize(0)
			submit = submit.Message(tail)
		}
		parts, err := submit.Timeout(timeout).Submit(context.Background())
		_ = payload.Close()
		if tail != nil {
			_ = tail.Close()
		}
		if err != nil {
			continue
		}
		reply, payloadErr := perfcommon.MeasurementPayload(parts)
		if payloadErr == nil {
			now := time.Now()
			if sent, valid := perfcommon.SentTimestampNsFromMessagePhase(reply, cfg.msgSize, perfcommon.PhaseActive); valid {
				stats.AddCount()
				stats.AddLatencySampleNs(float64(now.UnixNano() - sent))
			}
		}
		zlink.MultipartClose(parts)
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
