// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"fmt"
	"math"
	"testing"

	zlink "zlink.systems/zlink"
)

type hwmOptions interface {
	SetSendHighWaterMark(uint64) error
	SendHighWaterMark() (uint64, error)
	SetReceiveHighWaterMark(uint64) error
	ReceiveHighWaterMark() (uint64, error)
}

var (
	_ hwmOptions = (*zlink.PairSocket)(nil)
	_ hwmOptions = (*zlink.DealerSocket)(nil)
	_ hwmOptions = (*zlink.RouterSocket)(nil)
	_ hwmOptions = (*zlink.StreamSocket)(nil)
	_ hwmOptions = (*zlink.PubSocket)(nil)
	_ hwmOptions = (*zlink.SubSocket)(nil)
	_ hwmOptions = (*zlink.XPubSocket)(nil)
	_ hwmOptions = (*zlink.XSubSocket)(nil)
	_ hwmOptions = (*zlink.CommonSocketOptions)(nil)
)

func TestHighWaterMarkRoundTripsFullUint64Range(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	router, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer router.Close()
	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	for _, test := range []struct {
		name   string
		setter hwmOptions
		getter hwmOptions
	}{
		{"connection-to-common", router, router.CommonOptions()},
		{"common-to-connection", router.CommonOptions(), router},
		{"stream", stream, stream},
	} {
		for _, value := range []uint64{0, math.MaxInt64, uint64(math.MaxInt64) + 1, math.MaxUint64} {
			t.Run(fmt.Sprintf("%s/%d", test.name, value), func(t *testing.T) {
				if err := test.setter.SetSendHighWaterMark(value); err != nil {
					t.Fatalf("SetSendHighWaterMark(%d): %v", value, err)
				}
				if got, err := test.getter.SendHighWaterMark(); err != nil || got != value {
					t.Fatalf("SendHighWaterMark() = (%d, %v), want (%d, nil)", got, err, value)
				}
				if err := test.setter.SetReceiveHighWaterMark(value); err != nil {
					t.Fatalf("SetReceiveHighWaterMark(%d): %v", value, err)
				}
				if got, err := test.getter.ReceiveHighWaterMark(); err != nil || got != value {
					t.Fatalf("ReceiveHighWaterMark() = (%d, %v), want (%d, nil)", got, err, value)
				}
			})
		}
	}
}
