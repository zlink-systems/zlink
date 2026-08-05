package perfcommon

import (
	"encoding/binary"
	"sync/atomic"
	"time"

	zlink "zlink.systems/zlink/v11"
)

const (
	MetricMagic      uint32 = 0x5A4C4E4B
	MetricHeaderSize        = 29
	MetricRunID      uint32 = 1
	PhaseWarmup      uint8  = 0
	PhaseActive      uint8  = 1
	PhaseCooldown    uint8  = 2
)

var metricSequence uint64

func StampPayload(payload []byte) {
	StampPayloadPhase(payload, PhaseActive)
}

func StampProbePayload(payload []byte) {
	StampPayloadPhase(payload, PhaseWarmup)
}

func StampCooldownPayload(payload []byte) {
	StampPayloadPhase(payload, PhaseCooldown)
}

func StampPayloadPhase(payload []byte, phase uint8) {
	StampPayloadPhaseAt(payload, phase, time.Now())
}

func StampPayloadPhaseAt(payload []byte, phase uint8, now time.Time) {
	if len(payload) < MetricHeaderSize {
		Must(&invalidMetricPayloadError{Size: len(payload)})
	}
	binary.LittleEndian.PutUint32(payload[0:4], MetricMagic)
	binary.LittleEndian.PutUint32(payload[4:8], MetricRunID)
	payload[8] = phase
	binary.LittleEndian.PutUint32(payload[9:13], uint32(len(payload)))
	binary.LittleEndian.PutUint64(payload[13:21], atomic.AddUint64(&metricSequence, 1))
	binary.LittleEndian.PutUint64(payload[21:29], uint64(now.UnixNano()))
}

type MetricHeader struct {
	Magic    uint32
	RunID    uint32
	Phase    uint8
	MsgSize  uint32
	Sequence uint64
	SentTsNs int64
}

func DecodeMetricHeader(data []byte) (MetricHeader, bool) {
	if len(data) < MetricHeaderSize {
		return MetricHeader{}, false
	}
	return MetricHeader{
		Magic:    binary.LittleEndian.Uint32(data[0:4]),
		RunID:    binary.LittleEndian.Uint32(data[4:8]),
		Phase:    data[8],
		MsgSize:  binary.LittleEndian.Uint32(data[9:13]),
		Sequence: binary.LittleEndian.Uint64(data[13:21]),
		SentTsNs: int64(binary.LittleEndian.Uint64(data[21:29])),
	}, true
}

func validHeaderPhase(header MetricHeader, expectedMsgSize int, phase uint8) bool {
	return header.Magic == MetricMagic &&
		header.RunID == MetricRunID &&
		header.Phase == phase &&
		int(header.MsgSize) == expectedMsgSize
}

func SentAtFromBytes(data []byte, expectedMsgSize int) (time.Time, bool) {
	return SentAtFromBytesPhase(data, expectedMsgSize, PhaseActive)
}

func SentAtFromBytesPhase(data []byte, expectedMsgSize int, phase uint8) (time.Time, bool) {
	header, ok := DecodeMetricHeader(data)
	if !ok || !validHeaderPhase(header, expectedMsgSize, phase) {
		return time.Time{}, false
	}
	return time.Unix(0, header.SentTsNs), true
}

func SentAtFromMessage(part *zlink.Message, expectedMsgSize int) (time.Time, bool) {
	return SentAtFromMessagePhase(part, expectedMsgSize, PhaseActive)
}

func SentAtFromMessagePhase(part *zlink.Message, expectedMsgSize int, phase uint8) (time.Time, bool) {
	if part == nil {
		return time.Time{}, false
	}
	data := part.Data()
	return SentAtFromBytesPhase(data, expectedMsgSize, phase)
}

func SentTimestampNsFromBytesPhase(data []byte, expectedMsgSize int, phase uint8) (int64, bool) {
	header, ok := DecodeMetricHeader(data)
	if !ok || !validHeaderPhase(header, expectedMsgSize, phase) {
		return 0, false
	}
	return header.SentTsNs, true
}

func HasMetricHeaderPhase(data []byte, expectedMsgSize int, phase uint8) bool {
	header, ok := DecodeMetricHeader(data)
	return ok && validHeaderPhase(header, expectedMsgSize, phase)
}

func SentTimestampNsFromMessagePhase(part *zlink.Message, expectedMsgSize int, phase uint8) (int64, bool) {
	if part == nil {
		return 0, false
	}
	return SentTimestampNsFromBytesPhase(part.Data(), expectedMsgSize, phase)
}

func LatencyNsFromMessageAt(part *zlink.Message, expectedMsgSize int, phase uint8, now time.Time) (float64, bool) {
	sentTsNs, ok := SentTimestampNsFromMessagePhase(part, expectedMsgSize, phase)
	if !ok {
		return 0, false
	}
	nowNs := now.UnixNano()
	if nowNs < sentTsNs {
		return 0, false
	}
	return float64(nowNs - sentTsNs), true
}

func LatencyNsFromBytesAt(payload []byte, expectedMsgSize int, phase uint8, now time.Time) (float64, bool) {
	sentTsNs, ok := SentTimestampNsFromBytesPhase(payload, expectedMsgSize, phase)
	if !ok {
		return 0, false
	}
	nowNs := now.UnixNano()
	if nowNs < sentTsNs {
		return 0, false
	}
	return float64(nowNs - sentTsNs), true
}

func percentile(values []float64, pct float64) float64 {
	if len(values) == 0 {
		return 0
	}
	idx := int((pct / 100.0) * float64(len(values)-1))
	return values[idx]
}

type invalidMetricPayloadError struct {
	Size int
}

func (e *invalidMetricPayloadError) Error() string {
	return "perf payload must be >= 29 bytes"
}
