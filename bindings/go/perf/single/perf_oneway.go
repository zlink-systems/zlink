package main

import (
	"fmt"
	"os"
	"time"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

type recvSocket interface {
	zlink.SocketTarget
	Recv(*zlink.Received, zlink.RecvFlags) (bool, error)
}

func runSingleOneWay(
	cfg benchmarkConfig,
	receiver recvSocket,
	sendActive func(*zlink.Message) (bool, error),
	sendStop func(*zlink.Message) error,
) perfcommon.Result {
	return runSingleOneWayWithTransient(cfg, receiver, sendActive, sendStop, perfcommon.IsTransient)
}

func runSingleOneWayWithTransient(
	cfg benchmarkConfig,
	receiver recvSocket,
	sendActive func(*zlink.Message) (bool, error),
	sendStop func(*zlink.Message) error,
	isTransient func(error) bool,
) perfcommon.Result {
	if isTransient == nil {
		isTransient = perfcommon.IsTransient
	}
	if sendStop == nil {
		sendStop = func(message *zlink.Message) error {
			_, err := sendActive(message)
			return err
		}
	}
	window := perfcommon.NewBenchmarkWindow(cfg.duration)
	stats := perfcommon.NewStats()
	var received zlink.Received
	defer received.Close()

	// perf_single_one_way.hpp run_active_phase starts the receiver thread
	// before the sender thread and waits with a blocking recv. Only the
	// burst drain after the first payload uses DONTWAIT.
	receiverDone := make(chan error, 1)
	go func() {
		for {
			stop, drainErr := recvSingleOneWayUntilStop(receiver, &received, stats, cfg.msgSize, window.ActiveAt, window.StopAt)
			if drainErr != nil {
				receiverDone <- drainErr
				return
			}
			if stop {
				receiverDone <- nil
				return
			}
		}
	}()

	for time.Now().Before(window.StopAt) {
		message := perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt)
		sent, err := sendActive(message)
		if err != nil {
			_ = message.Close()
			if isTransient(err) {
				continue
			}
			if os.Getenv("PERF_DEBUG") != "" {
				fmt.Fprintf(os.Stderr, "single active send error: %v\n", err)
			}
			perfcommon.Must(err)
		}
		if !sent {
			_ = message.Close()
			continue
		}
	}
	// PERF_SINGLE_TEST_POLICY § 1.4: signal phase end via wire-level
	// stop token. Blocking send uses bounded transient-backpressure attempts
	// so the receiver always observes the terminator.
	if !sendStopTokenSingle(sendStop, isTransient) {
		perfcommon.Must(fmt.Errorf("single stop token send failed"))
	}
	if err := <-receiverDone; err != nil {
		perfcommon.Must(err)
	}

	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func recvSingleOneWayUntilStop(
	receiver recvSocket,
	received *zlink.Received,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
) (bool, error) {
	stop, _, err := recvSingleOneWayOnce(receiver, received, stats, msgSize, activeAt, stopAt, zlink.RecvFlagsNone)
	if err != nil || stop {
		return stop, err
	}
	return drainSingleOneWayUntilStop(receiver, received, stats, msgSize, activeAt, stopAt)
}

// drainSingleOneWayUntilStop drains the receiver until either a transient
// EAGAIN-style condition or the wire-level stop token arrives. It returns
// stop=true when the stop token has been observed.
func drainSingleOneWayUntilStop(
	receiver recvSocket,
	received *zlink.Received,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
) (bool, error) {
	for {
		stop, processed, err := recvSingleOneWayOnce(receiver, received, stats, msgSize, activeAt, stopAt, zlink.RecvFlagsDontWait)
		if err != nil || stop {
			return stop, err
		}
		if !processed {
			return false, nil
		}
	}
}

func recvSingleOneWayOnce(
	receiver recvSocket,
	received *zlink.Received,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
	flags zlink.RecvFlags,
) (bool, bool, error) {
	ok, err := receiver.Recv(received, flags)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false, false, nil
		}
		return false, false, err
	}
	if !ok {
		return false, false, nil
	}
	part, partErr := perfPayloadPart(received)
	if partErr != nil {
		return false, true, fmt.Errorf("unexpected multipart receive: %w", partErr)
	}
	if len(received.Parts()) != 1 {
		return false, true, fmt.Errorf("unexpected multipart receive in single one-way")
	}
	if perfcommon.IsStopTokenMessage(part) {
		return true, true, nil
	}
	if stats != nil {
		perfcommon.RecordMessageLatency(stats, activeAt, stopAt, msgSize, part)
	}
	return false, true, nil
}

// drainSingleOneWayProbe drains the receiver of pending messages once
// during ready-probe / phase-end barrier handshakes. It is not used by
// the active measurement loop (which goes through
// drainSingleOneWayUntilStop) but is kept for ready helpers that just
// need to know "did anything arrive."
func drainSingleOneWayProbe(receiver recvSocket) bool {
	processed := false
	var received zlink.Received
	for {
		ok, err := receiver.Recv(&received, zlink.RecvFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				return processed
			}
			perfcommon.Must(err)
		}
		if !ok {
			return processed
		}
		processed = true
		_ = received.Close()
	}
}

// sendStopTokenSingle attempts to push the wire-level stop token onto the
// connection. The send is bounded: each transient backpressure /
// EAGAIN response waits through `StopTokenSendBackoff`, capped by
// `StopTokenSendAttempts`. A non-transient error is fatal.
func sendStopTokenSingle(send func(*zlink.Message) error, isTransient func(error) bool) bool {
	if isTransient == nil {
		isTransient = perfcommon.IsTransient
	}
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		message := perfcommon.NewMessage(perfcommon.StopToken)
		err := send(message)
		if err == nil {
			return true
		}
		_ = message.Close()
		if !isTransient(err) {
			if os.Getenv("PERF_DEBUG") != "" {
				fmt.Fprintf(os.Stderr, "single stop token send error: %v\n", err)
			}
			return false
		}
		perfcommon.PollIdle(perfcommon.StopTokenSendBackoff)
	}
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintln(os.Stderr, "single stop token send: attempts exhausted")
	}
	return false
}

func waitSingleRouteReady(
	name string,
	send func([]byte) error,
	receiver recvSocket,
) {
	payload := perfcommon.PreparePayload(64)
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "%s ready probe poller create\n", name)
	}
	poller := perfcommon.NewSocketPoller(receiver, perfcommon.ZLinkPollIn)
	defer poller.Close()
	events := make([]zlink.PollEvent, 1)
	perfcommon.StampProbePayload(payload)
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "%s ready probe send\n", name)
	}
	if err := send(payload); err != nil {
		perfcommon.Must(fmt.Errorf("%s ready probe send: %w", name, err))
	}
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "%s ready probe sent\n", name)
	}
	deadline := time.Now().Add(perfcommon.SingleReadyTimeout())
	for time.Now().Before(deadline) {
		timeout := time.Until(deadline)
		if timeout <= 0 {
			break
		}
		event, err := perfcommon.WaitPollerOne(poller, events, timeout)
		if err != nil {
			if os.Getenv("PERF_DEBUG") != "" {
				fmt.Fprintf(os.Stderr, "%s ready probe poller wait error: %v\n", name, err)
			}
			perfcommon.Must(err)
		}
		if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
			if os.Getenv("PERF_DEBUG") != "" {
				fmt.Fprintf(os.Stderr, "%s ready probe poller wait timeout\n", name)
			}
			continue
		}
		if os.Getenv("PERF_DEBUG") != "" {
			fmt.Fprintf(os.Stderr, "%s ready probe poller readable\n", name)
		}
		if drainSingleOneWayProbe(receiver) {
			if os.Getenv("PERF_DEBUG") != "" {
				fmt.Fprintf(os.Stderr, "%s ready probe drained\n", name)
			}
			return
		}
	}
	perfcommon.Must(fmt.Errorf("%s did not become ready", name))
}

func perfPayloadPart(received *zlink.Received) (*zlink.Message, error) {
	if received == nil {
		return nil, nil
	}
	if received.IsSinglePart() {
		return received.SinglePartOrError()
	}
	parts := received.Parts()
	if received.HasRoutingID() && len(parts) > 0 {
		return parts[len(parts)-1], nil
	}
	return received.SinglePartOrError()
}
