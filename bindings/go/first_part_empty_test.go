// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"errors"
	"syscall"
	"testing"

	"zlink.systems/zlink"
)

func TestFirstPartEmptyResult(t *testing.T) {
	for _, tc := range []struct {
		name      string
		firstPart func() (*zlink.Message, error)
	}{
		{"Received", (&zlink.Received{}).FirstPart},
		{"TopicMessage", (&zlink.TopicMessage{}).FirstPart},
	} {
		t.Run(tc.name, func(t *testing.T) {
			part, err := tc.firstPart()
			var recvErr *zlink.RecvError
			if part != nil || !errors.As(err, &recvErr) {
				t.Fatalf("FirstPart() = (%v, %T), want (nil, *RecvError)", part, err)
			}
			if recvErr.Result != zlink.RecvInvalidState || !errors.Is(err, syscall.EINVAL) {
				t.Fatalf("FirstPart() error = result %v, errno %d, want RecvInvalidState, EINVAL",
					recvErr.Result, recvErr.InternalErrno())
			}
		})
	}
}
