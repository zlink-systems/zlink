// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"errors"
	"syscall"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestStreamReceiveModeConflicts(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	for _, tc := range []struct {
		name        string
		mode, other zlink.StreamReceiveMode
		wrongRecv   func(*zlink.StreamSocket) (bool, error)
	}{
		{"raw", zlink.StreamReceiveRaw, zlink.StreamReceivePacket, func(s *zlink.StreamSocket) (bool, error) {
			var out zlink.StreamPacket
			defer out.Close()
			ok, err := s.RecvPacket(&out, zlink.RecvFlagsDontWait)
			if !out.Empty() {
				t.Error("packet output was populated in RAW mode")
			}
			return ok, err
		}},
		{"packet", zlink.StreamReceivePacket, zlink.StreamReceiveRaw, func(s *zlink.StreamSocket) (bool, error) {
			var out zlink.Received
			defer out.Close()
			return s.Recv(&out, zlink.RecvFlagsDontWait)
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s, err := ctx.StreamSocket()
			if err != nil {
				t.Fatal(err)
			}
			defer s.Close()
			if err := s.SetReceiveMode(tc.mode); err != nil {
				t.Fatal(err)
			}
			if err := s.Bind("tcp://127.0.0.1:*"); err != nil {
				t.Fatal(err)
			}
			for _, mode := range []zlink.StreamReceiveMode{tc.mode, tc.other} {
				err := s.SetReceiveMode(mode)
				var config *zlink.ConfigError
				if !errors.As(err, &config) || config.Result != zlink.ConfigInvalidState || !errors.Is(err, syscall.EBUSY) {
					t.Errorf("SetReceiveMode(%v) after bind = %v, want invalid state (EBUSY)", mode, err)
				}
			}
			if mode, err := s.ReceiveMode(); err != nil || mode != tc.mode {
				t.Errorf("ReceiveMode = %v, %v, want %v", mode, err, tc.mode)
			}
			ok, err := tc.wrongRecv(s)
			var recv *zlink.RecvError
			if ok || !errors.As(err, &recv) || recv.Result != zlink.RecvNotSupported || !errors.Is(err, syscall.ENOTSUP) {
				t.Errorf("wrong receive = %t, %v, want not supported (ENOTSUP)", ok, err)
			}
		})
	}
}
