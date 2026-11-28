// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"reflect"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestRawSocketCapabilityMatrix(t *testing.T) {
	const (
		send = 1 << iota
		sendTo
		publish
		recv
		recvPacket
		subscribe
		subscriptionEvent
		subscriptionOptions
		request
		reply
	)
	for _, socket := range []struct {
		target       any
		outbound     bool
		capabilities int
	}{
		{(*zlink.PairSocket)(nil), true, send | recv},
		{(*zlink.PubSocket)(nil), true, publish},
		{(*zlink.SubSocket)(nil), true, subscribe | subscriptionOptions},
		{(*zlink.DealerSocket)(nil), true, send | recv | request},
		{(*zlink.RouterSocket)(nil), true, sendTo | recv | request | reply},
		{(*zlink.XPubSocket)(nil), true, publish | subscriptionEvent},
		{(*zlink.XSubSocket)(nil), true, subscribe | subscriptionOptions},
		{(*zlink.StreamSocket)(nil), false, sendTo | recv | recvPacket},
	} {
		t.Run(reflect.TypeOf(socket.target).Elem().Name(), func(t *testing.T) {
			expect := func(method string, want bool) {
				t.Helper()
				if got := hasMethod(socket.target, method); got != want {
					t.Errorf("%s present = %t, want %t", method, got, want)
				}
			}
			for _, method := range []string{"Bind", "Unbind", "DisconnectRID", "SetTLSServer", "SetTLSClient", "CommonOptions"} {
				expect(method, true)
			}
			for _, method := range []string{"Connect", "Disconnect", "ConnectionAttemptsSnapshot"} {
				expect(method, socket.outbound)
			}
			for _, method := range []string{"SetOption", "GetOption", "SetSockOpt", "GetSockOpt"} {
				expect(method, false)
			}
			for _, capability := range []struct {
				method string
				bit    int
			}{
				{"Send", send}, {"SendTo", sendTo}, {"Publish", publish}, {"Recv", recv},
				{"RecvPacket", recvPacket}, {"Subscribe", subscribe}, {"ReceiveSubscriptionEvent", subscriptionEvent},
				{"SetSubscription", subscriptionOptions}, {"UnsetSubscription", subscriptionOptions}, {"SubscriptionAt", subscriptionOptions},
				{"Request", request}, {"Reply", reply},
			} {
				expect(capability.method, socket.capabilities&capability.bit != 0)
			}
		})
	}
}
