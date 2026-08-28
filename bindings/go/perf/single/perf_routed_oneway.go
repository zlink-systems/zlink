package main

import (
	"fmt"
	"os"
	"runtime"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type routedRecvSocket interface {
	zlink.SocketTarget
	Recv(*zlink.Received, zlink.RecvFlags) (bool, error)
}

func runSingleRoutedOneWay(
	cfg benchmarkConfig,
	receiver routedRecvSocket,
	sendActive func(*zlink.Message) (bool, error),
	sendStop func(*zlink.Message) error,
) perfcommon.Result {
	return runSingleRoutedOneWayWithTransient(cfg, receiver, sendActive, sendStop, perfcommon.IsTransient)
}

func runSingleRoutedOneWayWithTransient(
	cfg benchmarkConfig,
	receiver routedRecvSocket,
	sendActive func(*zlink.Message) (bool, error),
	sendStop func(*zlink.Message) error,
	isTransient func(error) bool,
) perfcommon.Result {
	if isTransient == nil {
		isTransient = perfcommon.IsTransient
	}
	window := perfcommon.NewBenchmarkWindow(cfg.duration)
	stats := perfcommon.NewStats()

	senderDone := make(chan error, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		for time.Now().Before(window.StopAt) {
			message := perfcommon.NewWindowMessage(cfg.msgSize, window.ActiveAt)
			sent, err := sendActive(message)
			if err != nil {
				_ = message.Close()
				if isTransient(err) {
					continue
				}
				if os.Getenv("PERF_DEBUG") != "" {
					fmt.Fprintf(os.Stderr, "single routed active send error: %v\n", err)
				}
				senderDone <- err
				return
			}
			if !sent {
				_ = message.Close()
				continue
			}
		}
		if !sendStopTokenSingle(sendStop, isTransient) {
			senderDone <- fmt.Errorf("single routed stop token send failed")
			return
		}
		senderDone <- nil
	}()

	var received zlink.Received
	defer received.Close()

	recvErr := error(nil)
	for {
		stop, err := recvSingleRoutedOneWayOnce(receiver, &received, stats, cfg.msgSize)
		if err != nil {
			recvErr = err
			break
		}
		if stop {
			break
		}
	}
	if err := <-senderDone; err != nil {
		perfcommon.Must(err)
	}
	if recvErr != nil {
		perfcommon.Must(recvErr)
	}

	return stats.Snapshot(cfg.duration, cfg.msgSize)
}

func recvSingleRoutedOneWayOnce(
	receiver routedRecvSocket,
	received *zlink.Received,
	stats *perfcommon.Stats,
	msgSize int,
) (bool, error) {
	ok, err := receiver.Recv(received, zlink.RecvFlagsNone)
	if err != nil {
		if perfcommon.IsTransient(err) {
			return false, nil
		}
		return false, err
	}
	if !ok {
		return false, nil
	}
	if received.RoutingID().Size() == 0 || received.HasRequestSeq() {
		return false, fmt.Errorf("unexpected routed receive metadata")
	}
	parts := received.Parts()
	if len(parts) == 1 && perfcommon.IsStopTokenMessage(parts[0]) {
		return true, nil
	}
	part, partErr := perfcommon.MeasurementPayload(parts)
	if partErr != nil {
		return false, partErr
	}
	now := time.Now()
	sentTsNs, valid := perfcommon.SentTimestampNsFromMessagePhase(
		part, msgSize, perfcommon.PhaseActive)
	if valid {
		stats.AddCount()
		if nowNs := now.UnixNano(); nowNs >= sentTsNs {
			stats.AddLatencySampleNs(float64(nowNs - sentTsNs))
		}
	}
	return false, nil
}
