// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"errors"
	"syscall"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestStreamCommonOptions(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()

	socket, ok := any(stream).(interface {
		CommonOptions() *zlink.CommonSocketOptions
	})
	if !ok {
		t.Fatal("StreamSocket is missing CommonOptions() *CommonSocketOptions")
	}
	options := socket.CommonOptions()

	const sendHWM uint64 = 1 << 40
	if err := stream.SetSendHighWaterMark(sendHWM); err != nil {
		t.Fatal(err)
	}
	if got, err := options.SendHighWaterMark(); err != nil || got != sendHWM {
		t.Fatalf("facade SendHighWaterMark() = (%d, %v), want (%d, nil)", got, err, sendHWM)
	}

	const receiveHWM uint64 = 1 << 41
	if err := options.SetReceiveHighWaterMark(receiveHWM); err != nil {
		t.Fatal(err)
	}
	if got, err := stream.ReceiveHighWaterMark(); err != nil || got != receiveHWM {
		t.Fatalf("direct ReceiveHighWaterMark() = (%d, %v), want (%d, nil)", got, err, receiveHWM)
	}

	err = options.SetReceiveFlowState(zlink.ReceiveFlowPaused)
	var configErr *zlink.ConfigError
	if !errors.As(err, &configErr) || configErr.Result != zlink.ConfigNotSupported ||
		configErr.InternalErrno() != int(syscall.ENOTSUP) {
		t.Fatalf("facade SetReceiveFlowState() = %v, want ConfigError(NotSupported, ENOTSUP)", err)
	}
}
