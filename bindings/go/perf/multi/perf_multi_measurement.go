package main

import (
	"os"
	"strconv"
	"time"

	"zlink.systems/zlink/perf/internal/perfcommon"
)

const (
	maxBurstPerSender = 64
	receiveQuantum    = 1
	settle            = 500 * time.Millisecond
)

type multiSchedule struct {
	epoch                    time.Time
	rate                     float64
	reserved, accepted, late uint64
}

func newMultiSchedule(epoch time.Time, received uint64, active time.Duration) *multiSchedule {
	return &multiSchedule{epoch: epoch, rate: float64(received) / active.Seconds() * 0.8}
}

func (s *multiSchedule) next() time.Time {
	return s.epoch.Add(time.Duration(float64(s.reserved) * float64(time.Second) / s.rate))
}

func (s *multiSchedule) due(now time.Time) bool {
	return s.rate > 0 && !now.Before(s.next())
}

func (s *multiSchedule) reserve() time.Time {
	due := s.next()
	s.reserved++
	return due
}

func (s *multiSchedule) admit(due, now time.Time) {
	s.accepted++
	if now.Sub(due) > time.Millisecond {
		s.late++
	}
}

func (s *multiSchedule) scheduled(elapsed time.Duration) uint64 {
	return uint64(s.rate * max(0, elapsed.Seconds()))
}

func (s *multiSchedule) phase(now time.Time) uint8 {
	if now.Before(s.epoch.Add(settle)) {
		return perfcommon.PhaseWarmup
	}
	return perfcommon.PhaseLatency
}

func (s *multiSchedule) print(cfg multiConfig, activeSends, received, paced uint64, caughtUp bool, catchupSeconds float64) {
	caught := "no"
	if caughtUp {
		caught = "yes"
	}
	flushControlLine("BENCH_DIAG,%s,%d,active_sender_rate,%.12g,target_sender_rate,%.12g,latency_sends,%d,late_sends_gt_1ms,%d,received_rate,%.12g,paced_received_rate,%.12g,caught_up,%s,catch_up_seconds,%.12g",
		cfg.pattern, cfg.msgSize, float64(activeSends)/cfg.duration.Seconds(), s.rate, s.accepted, s.late,
		float64(received)/cfg.duration.Seconds(), float64(paced)/multiLatencyDuration(cfg.duration).Seconds(), caught, catchupSeconds)
}

func multiLatencyDuration(active time.Duration) time.Duration {
	if seconds, err := strconv.Atoi(os.Getenv("PERF_MULTI_LATENCY_DURATION_SECONDS")); err == nil && seconds > 0 {
		return time.Duration(seconds) * time.Second
	}
	return active
}

func multiWait(now, until time.Time) time.Duration {
	return min(50*time.Millisecond, max(0, until.Sub(now).Truncate(time.Millisecond)))
}

type multiCatchUp struct {
	start                              time.Duration
	received                           []uint64
	scheduled, startInFlight, inFlight uint64
}

func (t *multiCatchUp) observe(now time.Duration, received []uint64, scheduled uint64) bool {
	if len(received) == 0 {
		return false
	}
	minimum := minReceived(received)
	t.inFlight = scheduled - min(scheduled, minimum)
	if t.received != nil && t.inFlight <= t.startInFlight {
		if now-t.start < settle {
			return false
		}
		minimumDelta := received[0] - t.received[0]
		for i := 1; i < len(received); i++ {
			minimumDelta = min(minimumDelta, received[i]-t.received[i])
		}
		if minimumDelta+receiveQuantum >= scheduled-t.scheduled {
			return true
		}
	}
	t.start, t.startInFlight, t.scheduled = now, t.inFlight, scheduled
	t.received = append(t.received[:0], received...)
	return false
}

func minReceived(counts []uint64) uint64 {
	value := counts[0]
	for _, count := range counts[1:] {
		value = min(value, count)
	}
	return value
}

func recordMultiReceive(stats *perfcommon.Stats, payload []byte, size int, observed int64, window perfcommon.BenchmarkWindow, paced bool, divisor float64) bool {
	if !paced {
		if observed < window.StopAtNs && perfcommon.HasMetricHeaderPhase(payload, size, perfcommon.PhaseActive) {
			stats.AddCount()
		}
		return false
	}
	header, valid := perfcommon.DecodeMetricHeader(payload)
	if !valid || header.Magic != perfcommon.MetricMagic || header.RunID != perfcommon.MetricRunID ||
		header.MsgSize != uint32(size) || (header.Phase != perfcommon.PhaseWarmup && header.Phase != perfcommon.PhaseLatency) {
		return false
	}
	if header.Phase == perfcommon.PhaseLatency && header.SentTsNs > 0 && observed >= header.SentTsNs {
		stats.AddLatencyNs(float64(observed-header.SentTsNs) / divisor)
	}
	return observed >= window.ActiveAtNs+settle.Nanoseconds() && observed < window.StopAtNs
}
