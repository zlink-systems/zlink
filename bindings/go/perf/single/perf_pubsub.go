package main

import (
	"context"
	"fmt"
	"time"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

const singlePubSubTopic = "bench"

func runPubSub(cfg benchmarkConfig) perfcommon.Result {
	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()
	perfcommon.ApplySingleAutoHWMMsgUnit(ctx, cfg.msgSize)

	publisher, err := ctx.PubSocket()
	perfcommon.Must(err)
	defer publisher.Close()
	subscriber, err := ctx.SubSocket()
	perfcommon.Must(err)
	defer subscriber.Close()

	pubMon := perfcommon.OpenMonitor(publisher)
	defer pubMon.Close()
	subMon := perfcommon.OpenMonitor(subscriber)
	defer subMon.Close()

	perfcommon.Must(perfcommon.ConfigureTLSServer(publisher, cfg.transport))
	perfcommon.Must(perfcommon.ConfigureTLSClient(subscriber, cfg.transport))
	perfcommon.Must(publisher.SetNoDrop(true))
	perfcommon.ApplySingleHWM(publisher)
	perfcommon.ApplySingleHWM(subscriber)
	endpoint := perfcommon.BindAndResolveEndpoint(publisher, cfg.transport, "perf-pubsub")
	perfcommon.Must(subscriber.Connect(endpoint))
	perfcommon.ApplySingleBenchmarkSocketOptions(publisher, cfg.transport)
	perfcommon.ApplySingleBenchmarkSocketOptions(subscriber, cfg.transport)
	perfcommon.WaitConnectedWithTimeout(perfcommon.SingleReadyTimeout(), pubMon, subMon)

	perfcommon.Must(subscriber.SetSubscription(""))
	perfcommon.Must(subscriber.SetReceiveTimeout(perfcommon.SinglePubSubReceiveTimeout()))
	perfcommon.PostReadySettle(cfg.pattern)

	window := perfcommon.NewBenchmarkWindow(cfg.duration)
	stats := perfcommon.NewStats()
	var received zlink.TopicMessage
	defer received.Close()

	receiverDone := make(chan error, 1)
	go func() {
		for {
			stop, drainErr := recvSinglePubSubUntilStop(subscriber, &received, stats, cfg.msgSize, window.ActiveAt, window.StopAt)
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
		_, err := perfcommon.SubmitMessage(perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt), func(message *zlink.Message) (bool, error) {
			return publisher.Publish(singlePubSubTopic).MoveMessage(message).Flags(zlink.SendFlagsDontWait).Submit(context.Background())
		})
		if err != nil {
			if perfcommon.IsTransient(err) {
				continue
			}
			perfcommon.Must(err)
		}
	}
	// PERF_SINGLE_TEST_POLICY § 1.4: signal phase end via wire-level
	// stop token published on the same topic so the subscriber sees
	// it as the last message in the active stream.
	if !sendPubSubStopToken(publisher) {
		perfcommon.Must(fmt.Errorf("PUBSUB stop token send failed"))
	}
	if err := <-receiverDone; err != nil {
		perfcommon.Must(err)
	}

	result := stats.Snapshot(cfg.duration, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(pubMon, cfg.pattern, cfg.transport, "publisher", zlink.SocketTypePub, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(subMon, cfg.pattern, cfg.transport, "subscriber", zlink.SocketTypeSub, cfg.msgSize)
	return result
}

func recvSinglePubSubUntilStop(
	subscriber *zlink.SubSocket,
	received *zlink.TopicMessage,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
) (bool, error) {
	stop, _, err := recvSinglePubSubOnce(subscriber, received, stats, msgSize, activeAt, stopAt, zlink.RecvFlagsNone)
	if err != nil || stop {
		return stop, err
	}
	return drainSinglePubSubUntilStop(subscriber, received, stats, msgSize, activeAt, stopAt)
}

// drainSinglePubSubUntilStop drains the subscriber until either a
// transient EAGAIN-style condition or the wire-level stop token arrives.
// It returns stop=true when the stop token has been observed.
func drainSinglePubSubUntilStop(
	subscriber *zlink.SubSocket,
	received *zlink.TopicMessage,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
) (bool, error) {
	for {
		stop, processed, err := recvSinglePubSubOnce(subscriber, received, stats, msgSize, activeAt, stopAt, zlink.RecvFlagsDontWait)
		if err != nil || stop {
			return stop, err
		}
		if !processed {
			return false, nil
		}
	}
}

func recvSinglePubSubOnce(
	subscriber *zlink.SubSocket,
	received *zlink.TopicMessage,
	stats *perfcommon.Stats,
	msgSize int,
	activeAt time.Time,
	stopAt time.Time,
	flags zlink.RecvFlags,
) (bool, bool, error) {
	ok, err := subscriber.Subscribe(received, flags)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false, false, nil
		}
		return false, false, err
	}
	if !ok {
		return false, false, nil
	}
	if received.Topic() != singlePubSubTopic || len(received.Parts()) != 1 {
		return false, true, fmt.Errorf("unexpected PUBSUB metadata topic=%q parts=%d", received.Topic(), len(received.Parts()))
	}
	part, partErr := received.SinglePartOrError()
	if partErr != nil {
		return false, true, partErr
	}
	if perfcommon.IsStopTokenMessage(part) {
		return true, true, nil
	}
	perfcommon.RecordMessageLatency(stats, activeAt, stopAt, msgSize, part)
	return false, true, nil
}

// sendPubSubStopToken pushes the wire-level stop token on the bench
// topic. Bounded attempts through transient backpressure mirror the
// pattern used in single one-way. The token is published on the
// same `bench` topic so it is delivered to subscribers that
// matched the active stream.
func sendPubSubStopToken(publisher *zlink.PubSocket) bool {
	for attempt := 0; attempt < perfcommon.StopTokenSendAttempts; attempt++ {
		_, err := perfcommon.SubmitMessage(perfcommon.NewMessage(perfcommon.StopToken), func(message *zlink.Message) (bool, error) {
			return publisher.Publish(singlePubSubTopic).MoveMessage(message).Submit(context.Background())
		})
		if err == nil {
			return true
		}
		if !perfcommon.IsTransient(err) {
			return false
		}
		perfcommon.PollIdle(perfcommon.StopTokenSendBackoff)
	}
	return false
}
