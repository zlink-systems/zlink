// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"reflect"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestTypedSocketOptionsStayWithTheirRoles(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	router, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer dealer.Close()
	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()

	for _, option := range []struct {
		name string
		set  func(bool) error
		get  func() (bool, error)
	}{
		{"router mandatory", router.SetMandatory, router.Mandatory},
		{"router handover", router.SetHandover, router.Handover},
		{"router probe", router.SetProbe, router.Probe},
		{"dealer probe", dealer.SetProbe, dealer.Probe},
		{"stream notify", stream.SetNotify, stream.Notify},
	} {
		t.Run(option.name, func(t *testing.T) {
			for _, want := range []bool{true, false} {
				if err := option.set(want); err != nil {
					t.Fatal(err)
				}
				if got, err := option.get(); err != nil || got != want {
					t.Errorf("getter returned %t, %v, want %t", got, err, want)
				}
			}
		})
	}
	for _, target := range []any{router, dealer, stream, (*zlink.PairSocket)(nil), (*zlink.PubSocket)(nil),
		(*zlink.SubSocket)(nil), (*zlink.XPubSocket)(nil), (*zlink.XSubSocket)(nil)} {
		for _, tc := range []struct {
			allowed bool
			names   []string
		}{
			{target == router, []string{"Mandatory", "SetMandatory", "Handover", "SetHandover", "SetConnectRoutingID"}},
			{target == router || target == dealer, []string{"Probe", "SetProbe"}},
			{target == stream, []string{"Notify", "SetNotify", "ReceiveMode", "SetReceiveMode"}},
		} {
			for _, name := range tc.names {
				if got := hasMethod(target, name); got != tc.allowed {
					t.Errorf("%s.%s present = %t, want %t", reflect.TypeOf(target), name, got, tc.allowed)
				}
			}
		}
	}
}
