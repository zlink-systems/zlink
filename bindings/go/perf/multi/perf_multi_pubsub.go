package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

// Match the limits of C's send_stop_token_bounded.
const (
	perfStopSendTimeoutMS   = 2000
	perfStopSendMaxAttempts = 5
)

func runMultiPubSubServer(cfg multiConfig) {
	ctx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)
	defer ctx.Close()

	publisher, err := ctx.PubSocket()
	perfcommon.Must(err)
	defer publisher.Close()
	perfcommon.Must(perfcommon.ConfigureTLSServer(publisher, cfg.transport))
	perfcommon.ApplyMultiHWM(publisher, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(publisher, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(publisher, cfg.transport, "perf-multi-pubsub")
	flushControlLine("READY,%s", endpoint)
	if !waitForStartToken(cfg.msgSize) {
		return
	}
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate the context auto-HWM once the
	// subscribers are connected, like the C reference
	// (bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:316).
	perfcommon.Must(ctx.RecalculateAutoHwm())

	publish := func(phase uint8) bool {
		message := perfcommon.NewMessageWithSize(cfg.msgSize)
		perfcommon.StampPayloadPhase(message.Data(), phase)
		sent, err := perfcommon.SubmitMeasurement(publisher.Publish("bench"), message, zlink.SendFlagsNone)
		if err != nil && !perfcommon.IsTransient(err) {
			perfcommon.Must(fmt.Errorf("multi pubsub publish: %w", err))
		}
		return sent && err == nil
	}
	done := func(phase uint8) {
		options := publisher.CommonOptions()
		sendTimeout, err := options.SendTimeout()
		perfcommon.Must(err)
		publishMarker := func(marker *zlink.Message) (bool, error) {
			return perfcommon.SubmitMeasurement(publisher.Publish("bench"), marker, zlink.SendFlagsNone)
		}
		err = publishMultiPubSubPhaseMarker(cfg.msgSize, phase, options.SetSendTimeout, publishMarker)
		if err != nil {
			perfcommon.Must(fmt.Errorf("multi pubsub publish phase marker: %w", err))
		}
		perfcommon.Must(options.SetSendTimeout(sendTimeout))
		flushControlLine("PHASE_DONE,%d,%d", cfg.msgSize, phase)
	}
	end := time.Now().Add(cfg.duration)
	var activeSends uint64
	for time.Now().Before(end) {
		if publish(perfcommon.PhaseActive) && time.Now().Before(end) {
			activeSends++
		}
	}
	perfcommon.PrintMultiSocketAutoHWMDetail(
		publisher, nil, cfg.pattern, cfg.transport, "server", "endpoint", zlink.SocketTypePub, cfg.msgSize,
	)
	done(perfcommon.PhaseActive)
	received := readPhaseCount("PHASE_PACED", cfg.msgSize)
	control := pubSubPhaseControl()
	schedule := newMultiSchedule(time.Now(), received, cfg.duration)
	latencyDuration := multiLatencyDuration(cfg.duration)
	catchDeadline := schedule.epoch.Add(latencyDuration)
	end = catchDeadline.Add(latencyDuration)
	caughtUp := false
	catchupSeconds := -1.0
	for time.Now().Before(end) {
		select {
		case command := <-control:
			if command.line == fmt.Sprintf("PHASE_CATCHUP,%d", cfg.msgSize) && !caughtUp && !command.when.After(catchDeadline) {
				caughtUp = true
				catchupSeconds = command.when.Sub(schedule.epoch).Seconds()
				end = command.when.Add(latencyDuration)
			}
		default:
		}
		now := time.Now()
		if !now.Before(end) {
			break
		}
		if schedule.due(now) {
			phase := perfcommon.PhaseWarmup
			if caughtUp {
				phase = perfcommon.PhaseLatency
			}
			if publish(phase) {
				schedule.admit(schedule.reserve(), time.Now())
			}
		} else {
			until := schedule.next()
			if end.Before(until) {
				until = end
			}
			time.Sleep(multiWait(now, until))
		}
	}
	done(perfcommon.PhaseLatency)
	prefix := fmt.Sprintf("PHASE_COMPLETE,%d,", cfg.msgSize)
	deadline := time.After(10 * time.Second)
	for {
		select {
		case <-deadline:
			perfcommon.Must(fmt.Errorf("multi pubsub completion timed out"))
		case command := <-control:
			if value, ok := strings.CutPrefix(command.line, prefix); ok {
				paced, err := strconv.ParseUint(value, 10, 64)
				perfcommon.Must(err)
				schedule.print(cfg, activeSends, received, paced, caughtUp, catchupSeconds)
				if schedule.late > 0 {
					flushControlLine("REFERENCE,current,PUBSUB,%s,%d,sender_late,%d", cfg.transport, cfg.msgSize, schedule.late)
				}
				return
			}
		}
	}
}

func publishMultiPubSubPhaseMarker(
	msgSize int,
	phase uint8,
	setSendTimeout func(time.Duration) error,
	publish func(*zlink.Message) (bool, error),
) error {
	if err := setSendTimeout(perfStopSendTimeoutMS * time.Millisecond); err != nil {
		return err
	}
	for attempt := 0; attempt < perfStopSendMaxAttempts; attempt++ {
		marker := perfcommon.NewMessageWithSize(perfcommon.MetricHeaderSize)
		perfcommon.StampPhaseMarker(marker.Data(), msgSize, phase)
		sent, err := perfcommon.SubmitMessage(marker, publish)
		if err != nil && !perfcommon.IsTransient(err) {
			return err
		}
		if sent && err == nil {
			return nil
		}
	}
	return syscall.ETIMEDOUT
}

type pubSubCommand struct {
	line string
	when time.Time
}

func pubSubPhaseControl() <-chan pubSubCommand {
	commands := make(chan pubSubCommand, 2)
	go func() {
		scanner := bufio.NewScanner(os.Stdin)
		for scanner.Scan() {
			commands <- pubSubCommand{scanner.Text(), time.Now()}
		}
	}()
	return commands
}

func runMultiPubSubClient(cfg multiConfig, endpoint string) perfcommon.Result {
	ctx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer ctx.Close()

	stats := perfcommon.NewMultiStats()
	subs := make([]*zlink.SubSocket, 0, cfg.clients)
	monitors := make([]*zlink.SocketMonitor, 0, cfg.clients)
	for i := 0; i < cfg.clients; i++ {
		sub, err := ctx.SubSocket()
		if err != nil {
			perfcommon.Must(fmt.Errorf("multi pubsub create sub socket[%d]: %w", i, err))
		}
		subs = append(subs, sub)
		perfcommon.Must(perfcommon.ConfigureTLSClient(sub, cfg.transport))
		perfcommon.ApplyMultiHWM(sub, cfg.pattern)
		perfcommon.ApplyMultiBenchmarkSocketOptions(sub, cfg.transport)
		subMon := perfcommon.OpenMonitor(sub)
		if err := sub.SetSubscription("bench"); err != nil {
			perfcommon.Must(fmt.Errorf("multi pubsub subscribe[%d]: %w", i, err))
		}
		if err := sub.Connect(endpoint); err != nil {
			perfcommon.Must(fmt.Errorf("multi pubsub connect sub[%d]: %w", i, err))
		}
		monitors = append(monitors, subMon)
	}
	for _, monitor := range monitors {
		perfcommon.WaitConnectedWithTimeout(perfcommon.MultiReadyTimeout(), monitor)
	}
	// PERF_MULTI_TEST_POLICY § 1.6: recalculate after target connections ready.
	perfcommon.Must(ctx.RecalculateAutoHwm())
	defer func() {
		for _, monitor := range monitors {
			_ = monitor.Close()
		}
		for _, sub := range subs {
			_ = sub.Close()
		}
	}()
	received := make([]*zlink.TopicMessage, len(subs))
	for i := range received {
		received[i] = &zlink.TopicMessage{}
	}
	defer func() {
		for _, message := range received {
			_ = message.Close()
		}
	}()

	flushControlLine("CLIENT_READY,%d", cfg.msgSize)
	if !waitForStartToken(cfg.msgSize) {
		return stats.Snapshot(cfg.duration, cfg.msgSize)
	}
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	for i, sub := range subs {
		perfcommon.Must(poller.AddSocket(sub, perfcommon.ZLinkPollIn, uintptr(i)))
	}

	events := make([]zlink.PollEvent, len(subs))
	commands := pubSubPhaseControl()
	activeCounts := make([]uint64, len(subs))
	pacedCounts := make([]uint64, len(subs))
	sampleCounts := make([]uint64, len(subs))
	latencyDuration := multiLatencyDuration(cfg.duration)
	var tracker multiCatchUp
	reference := perfcommon.NewMultiStats()
	inflightFloor := ^uint64(0)
	var nextProgress time.Duration
	var epochNs int64
	caughtUp := false
	receivePhase := func(phase uint8, duration time.Duration, schedule *multiSchedule) {
		window := activeDeadline(duration)
		limit := window.StopAt.Add(10 * time.Second)
		lastPayload := window.ActiveAt
		done := time.Time{}
		stopped := make([]bool, len(subs))
		stopCount := 0
		for time.Now().Before(limit) {
			select {
			case command := <-commands:
				if command.line == fmt.Sprintf("PHASE_DONE,%d,%d", cfg.msgSize, phase) {
					done = command.when
				}
			default:
			}
			if !done.IsZero() && (stopCount == len(subs) || (time.Since(done) >= 50*time.Millisecond && time.Since(lastPayload) >= 50*time.Millisecond)) {
				return
			}
			n, err := poller.Wait(events, 10*time.Millisecond)
			if err != nil && !perfcommon.IsTransient(err) {
				perfcommon.Must(err)
			}
			for _, event := range events[:n] {
				index := int(event.Slot)
				if event.Revents&perfcommon.ZLinkPollIn == 0 || index >= len(subs) {
					continue
				}
				for range maxBurstPerSender {
					ok, err := subs[index].Subscribe(received[index], zlink.RecvFlagsDontWait)
					if err != nil && !perfcommon.IsTransient(err) {
						perfcommon.Must(err)
					}
					if !ok || err != nil {
						break
					}
					lastPayload = time.Now()
					nowNs := perfcommon.MonotonicNowNs()
					parts := received[index].Parts()
					part, err := perfcommon.MeasurementPayload(parts)
					perfcommon.Must(err)
					payload := part.Data()
					header, ok := perfcommon.DecodeMetricHeader(payload)
					if !ok || header.Magic != perfcommon.MetricMagic || header.RunID != perfcommon.MetricRunID ||
						header.MsgSize != uint32(cfg.msgSize) {
						continue
					}
					if len(payload) == perfcommon.MetricHeaderSize && header.Sequence == 0 && header.SentTsNs == 0 {
						if header.Phase == phase && !stopped[index] {
							stopped[index] = true
							stopCount++
						}
						continue
					}
					if schedule == nil {
						if nowNs < window.StopAtNs && perfcommon.HasMetricHeaderPhase(payload, cfg.msgSize, perfcommon.PhaseActive) {
							activeCounts[index]++
						}
						continue
					}
					if header.Phase != perfcommon.PhaseWarmup && header.Phase != perfcommon.PhaseLatency {
						continue
					}
					sent, ok := perfcommon.SentTimestampNsFromBytesPhase(payload, cfg.msgSize, header.Phase)
					if !ok {
						continue
					}
					pacedCounts[index]++
					if epochNs == 0 {
						epochNs = sent
					}
					if sent > 0 && nowNs >= sent {
						if !caughtUp {
							reference.AddLatencyNs(float64(nowNs - sent))
						} else if header.Phase == perfcommon.PhaseLatency {
							sampleCounts[index]++
							stats.AddLatencyNs(float64(nowNs - sent))
						}
					}
				}
			}
			if schedule != nil && epochNs != 0 && !caughtUp {
				elapsed := time.Duration(perfcommon.MonotonicNowNs() - epochNs)
				scheduled := schedule.scheduled(elapsed)
				if elapsed >= nextProgress {
					minimum := minReceived(pacedCounts)
					floor := scheduled - min(scheduled, minimum)
					inflightFloor = min(inflightFloor, floor)
					flushControlLine("PUBSUB_CATCHUP_PROGRESS,%d,%.6g,%d,%d,%d,%.6g",
						cfg.msgSize, elapsed.Seconds(), floor, minimum, scheduled, schedule.rate)
					nextProgress = elapsed + time.Second
				}
				if elapsed <= latencyDuration && tracker.observe(elapsed, pacedCounts, scheduled, schedule.rate) {
					caughtUp = true
					flushControlLine("PHASE_CATCHUP,%d", cfg.msgSize)
					flushControlLine("PUBSUB_CATCHUP_DIAG,%d,%.6g,%d,window_start_in_flight,%d", cfg.msgSize, elapsed.Seconds(), tracker.inFlight, tracker.startInFlight)
				}
			}
		}
		perfcommon.Must(fmt.Errorf("multi pubsub receive drain timed out"))
	}
	receivePhase(perfcommon.PhaseActive, cfg.duration, nil)
	minimum := minReceived(activeCounts)
	if minimum == 0 {
		return stats.Snapshot(cfg.duration, cfg.msgSize)
	}
	flushControlLine("PHASE_DRAINED,%d,%d", cfg.msgSize, minimum)
	schedule := newMultiSchedule(time.Time{}, minimum, cfg.duration)
	receivePhase(perfcommon.PhaseLatency, 2*latencyDuration, schedule)
	flushControlLine("PHASE_COMPLETE,%d,%d", cfg.msgSize, minReceived(sampleCounts))
	result := stats.Snapshot(cfg.duration, cfg.msgSize)
	var activeTotal uint64
	for _, count := range activeCounts {
		activeTotal += count
	}
	result.Throughput = float64(activeTotal) / cfg.duration.Seconds()
	result.Bandwidth = result.Throughput * float64(cfg.msgSize) / 1e6
	if result.Valid {
		flushControlLine("PUBSUB_LATENCY_DIAG,%d,latency_p50_ms,%.6f", cfg.msgSize, result.LatencyP50Ns/1e6)
	} else {
		result.LatencyInvalid = "receiver_not_caught_up_by_deadline"
		ref := reference.Snapshot(cfg.duration, cfg.msgSize)
		if inflightFloor == ^uint64(0) {
			inflightFloor = 0
		}
		key := fmt.Sprintf("REFERENCE,current,PUBSUB,%s,%d", cfg.transport, cfg.msgSize)
		flushControlLine("%s,latency_ref_p50,%.6f", key, ref.LatencyP50Ns/1e6)
		flushControlLine("%s,latency_ref_p99,%.6f", key, ref.LatencyP99Ns/1e6)
		flushControlLine("%s,inflight_floor,%d", key, inflightFloor)
		flushControlLine("%s,inflight_floor_ms,%.6f", key, float64(inflightFloor)/schedule.rate*1000)
	}
	perfcommon.PrintMultiSocketAutoHWMDetail(
		subs[0], monitors[0], cfg.pattern, cfg.transport, "client", "endpoint", zlink.SocketTypeSub, cfg.msgSize,
	)
	flushControlLine("CLIENT_DONE,%d", cfg.msgSize)
	return result
}
