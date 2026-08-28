package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
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

	window := activeDeadline(cfg.duration)
	payload := perfcommon.PreparePayload(cfg.msgSize)
	for time.Now().Before(window.StopAt) {
		if useMultiPubSubWindowMessage(cfg.transport, cfg.msgSize) {
			_, err := perfcommon.SubmitMeasurement(publisher.Publish("bench"),
				perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt), zlink.SendFlagsDontWait)
			if err != nil {
				if perfcommon.IsTransient(err) {
					continue
				}
				perfcommon.Must(fmt.Errorf("multi pubsub publish size=%d clients=%d transport=%s: %w",
					cfg.msgSize, cfg.clients, cfg.transport, err))
			}
			continue
		}
		perfcommon.StampWindowPayload(payload, window.ActiveAt)
		msg := perfcommon.NewMessage(payload)
		_, err = perfcommon.SubmitMeasurement(publisher.Publish("bench"), msg, zlink.SendFlagsDontWait)
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi pubsub publish size=%d clients=%d transport=%s: %w",
				cfg.msgSize, cfg.clients, cfg.transport, err))
		}
	}
	sendMultiPubSubStopToken(publisher)
	// Keep the publisher/context open until the role runner sends STOP. The
	// terminal publish must have time to flush to every subscriber while
	// socket linger remains zero, matching the other multi-PUBSUB bindings.
	waitForStopToken()
}

func useMultiPubSubWindowMessage(transport string, msgSize int) bool {
	switch transport {
	case "tls":
		return msgSize <= 256
	case "wss":
		return msgSize == 64
	default:
		return false
	}
}

func runMultiPubSubClient(cfg multiConfig, endpoint string) perfcommon.Result {
	ctx, err := perfcommon.NewMultiClientContext()
	perfcommon.Must(err)
	defer ctx.Close()

	stats := perfcommon.NewStats()
	latencyStride := resolveMultiPubSubLatencySampleStride()
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
		_ = monitor.Close()
	}
	defer func() {
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
	window := activeDeadline(cfg.duration)
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	for i, sub := range subs {
		perfcommon.Must(poller.AddSocket(sub, perfcommon.ZLinkPollIn, uintptr(i)))
	}

	// perf_multi_pubsub_client.cpp run_recv_duration: the wire stop token is
	// the normal phase boundary. If PUB drops that terminal message for a
	// subscriber, the same bounded cooldown used by the Python runner lets a
	// client that observed active traffic finish without an infinite wait.
	phaseDone := false
	activeObserved := false
	stopWaitDeadline := window.StopAt.Add(6 * time.Second)
	events := make([]zlink.PollEvent, len(subs))
	for !phaseDone {
		n, err := poller.Wait(events, time.Second)
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(fmt.Errorf("multi pubsub client poll: %w", err))
		}
		if n == 0 {
			if activeObserved && time.Now().After(stopWaitDeadline) {
				phaseDone = true
			}
			continue
		}
		drainMultiPubSubReady(subs, received, events[:n], stats, cfg.msgSize, window.StopAt, latencyStride, &phaseDone, &activeObserved)
	}
	flushControlLine("CLIENT_DONE,%d", cfg.msgSize)
	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func drainMultiPubSubReady(
	subs []*zlink.SubSocket,
	received []*zlink.TopicMessage,
	events []zlink.PollEvent,
	stats *perfcommon.Stats,
	msgSize int,
	recvStopAt time.Time,
	latencyStride uint64,
	phaseDone *bool,
	activeObserved *bool,
) {
	for _, event := range events {
		if event.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		index := int(event.Slot)
		if index < 0 || index >= len(subs) {
			continue
		}
		drainMultiPubSubSocket(index, subs[index], received[index], stats, msgSize, recvStopAt, latencyStride, phaseDone, activeObserved)
		if *phaseDone {
			return
		}
	}
}

func drainMultiPubSubSocket(
	index int,
	socket *zlink.SubSocket,
	received *zlink.TopicMessage,
	stats *perfcommon.Stats,
	msgSize int,
	recvStopAt time.Time,
	latencyStride uint64,
	phaseDone *bool,
	activeObserved *bool,
) {
	// zlink_subscribe_part may need to finish a multipart publish after the
	// poller reports its first frame. Receive one complete publish per event;
	// the next poller wakeup continues draining queued messages without making
	// a no-data DONT_WAIT call in the middle of a multipart sequence.
	ok, err := socket.Subscribe(received, zlink.RecvFlagsDontWait)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return
		}
		perfcommon.Must(fmt.Errorf("multi pubsub subscribe[%d]: %w", index, err))
	}
	if !ok {
		return
	}
	if received.Topic() != "bench" {
		perfcommon.Must(fmt.Errorf("multi pubsub unexpected metadata[%d]: topic=%q parts=%d", index, received.Topic(), len(received.Parts())))
	}
	parts := received.Parts()
	if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
		*phaseDone = true
		return
	}
	part, partErr := perfcommon.MeasurementPayload(parts)
	if partErr != nil {
		perfcommon.Must(fmt.Errorf("multi pubsub part[%d]: %w", index, partErr))
	}
	if !perfcommon.HasMetricHeaderPhase(part.Data(), msgSize, perfcommon.PhaseActive) {
		return
	}
	count := stats.AddCount()
	*activeObserved = true
	if shouldSampleMultiPubSubLatency(count, latencyStride) {
		now := time.Now()
		if latencyNs, ok := perfcommon.LatencyNsFromMessageAt(part, msgSize, perfcommon.PhaseActive, now); ok && now.Before(recvStopAt) {
			stats.AddLatencySampleNs(latencyNs)
		}
	}
}

func resolveMultiPubSubLatencySampleStride() uint64 {
	return uint64(positiveMultiPubSubIntEnv("PERF_MULTI_PUBSUB_LATENCY_SAMPLE_STRIDE", 32))
}

func positiveMultiPubSubIntEnv(name string, fallback int) int {
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return value
}

func shouldSampleMultiPubSubLatency(index, stride uint64) bool {
	return stride <= 1 || index == 1 || index%stride == 0
}

func sendMultiPubSubStopToken(publisher *zlink.PubSocket) {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		sent, err := perfcommon.SubmitPayload(perfcommon.StopToken, func(message *zlink.Message) (bool, error) {
			return publisher.Publish("bench").Message(message).Flags(zlink.SendFlagsNone).Submit(context.Background())
		})
		if err == nil && sent {
			return
		}
		if err != nil && !perfcommon.IsTransient(err) {
			return
		}
		perfcommon.PollIdle(perfcommon.StopTokenSendBackoff)
	}
}
