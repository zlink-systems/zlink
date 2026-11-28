package main

import (
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type multiEchoSocket interface {
	zlink.SocketTarget
	Recv(*zlink.Received, zlink.RecvFlags) (bool, error)
}

func replyTurn(replies *int) bool {
	*replies++
	if *replies < maxBurstPerSender {
		return false
	}
	*replies = 0
	return true
}

func runMultiEcho(cfg multiConfig, sockets []multiEchoSocket, send func(int, *zlink.Message) (zlink.SendSubmission, error)) perfcommon.Result {
	poller, err := zlink.NewPoller()
	perfcommon.Must(err)
	defer poller.Close()
	events := make([]zlink.PollEvent, len(sockets))
	for i, socket := range sockets {
		perfcommon.Must(poller.AddSocket(socket, perfcommon.ZLinkPollIn|zlink.PollCompletion, uintptr(i)))
	}
	payload := perfcommon.PreparePayload(cfg.msgSize)
	run := func(window perfcommon.BenchmarkWindow, schedule *multiSchedule) (uint64, uint64, perfcommon.Result) {
		stats := perfcommon.NewMultiStats()
		submit := func(index int) (zlink.SendSubmission, error) {
			phase := perfcommon.PhaseActive
			if schedule != nil {
				phase = schedule.phase(time.Now())
			}
			perfcommon.StampPayloadPhase(payload, phase)
			return send(index, perfcommon.NewMessage(payload))
		}
		pending, replies := 0, 0
		var pacedReceived uint64
		progress := func(wait time.Duration, submitDue func() error) error {
			n, err := poller.Wait(events, wait)
			if err != nil && !perfcommon.IsTransient(err) {
				return err
			}
			for _, event := range events[:n] {
				if event.Revents&perfcommon.ZLinkPollIn == 0 || int(event.Slot) >= len(sockets) {
					continue
				}
				var received zlink.Received
				for range maxBurstPerSender {
					ok, err := sockets[event.Slot].Recv(&received, zlink.RecvFlagsDontWait)
					if err != nil && !perfcommon.IsTransient(err) {
						return err
					}
					if !ok || err != nil {
						break
					}
					part, err := perfcommon.MeasurementPayload(received.Parts())
					if err == nil {
						if recordMultiReceive(stats, part.Data(), cfg.msgSize, perfcommon.MonotonicNowNs(), window, schedule != nil, 2) {
							pacedReceived++
						}
						pending--
					}
					_ = received.Close()
					if schedule != nil && replyTurn(&replies) {
						if err := submitDue(); err != nil {
							return err
						}
					}
				}
			}
			return nil
		}
		accepted, err := runMultiSendTurns(len(sockets), window, schedule, cfg.pattern, submit, progress,
			func(count int) { pending += count }, func() bool { return pending > 0 })
		perfcommon.Must(err)
		return accepted, pacedReceived, stats.Snapshot(cfg.duration, cfg.msgSize)
	}

	activeSends, _, active := run(activeDeadline(cfg.duration), nil)
	if active.Count == 0 {
		return active
	}
	window := activeDeadline(settle + multiLatencyDuration(cfg.duration))
	schedule := newMultiSchedule(window.ActiveAt, active.Count, cfg.duration)
	_, pacedReceived, paced := run(window, schedule)
	schedule.print(cfg, activeSends, active.Count, pacedReceived, false, -1)
	paced.Throughput, paced.Bandwidth = active.Throughput, active.Bandwidth
	return paced
}
