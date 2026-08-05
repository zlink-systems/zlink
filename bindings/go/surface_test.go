package zlink_test

import (
	"reflect"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

var (
	_ zlink.SocketTarget = (*zlink.PairSocket)(nil)
	_ zlink.SocketTarget = (*zlink.PubSocket)(nil)
	_ zlink.SocketTarget = (*zlink.SubSocket)(nil)
	_ zlink.SocketTarget = (*zlink.DealerSocket)(nil)
	_ zlink.SocketTarget = (*zlink.RouterSocket)(nil)
	_ zlink.SocketTarget = (*zlink.XPubSocket)(nil)
	_ zlink.SocketTarget = (*zlink.XSubSocket)(nil)
	_ zlink.SocketTarget = (*zlink.StreamSocket)(nil)
)

func hasMethod(target any, name string) bool {
	_, ok := reflect.TypeOf(target).MethodByName(name)
	return ok
}

func methodType(target any, name string) reflect.Type {
	method, ok := reflect.TypeOf(target).MethodByName(name)
	if !ok {
		return nil
	}
	return method.Type
}

func TestSurfaceCanonicalBuilderSignatures(t *testing.T) {
	assertReturn := func(target any, name string, want reflect.Type) {
		t.Helper()
		method := methodType(target, name)
		if method == nil {
			t.Fatalf("%T should expose %s", target, name)
		}
		if method.NumOut() != 1 || method.Out(0) != want {
			t.Fatalf("%T.%s return = %v, want %v", target, name, method, want)
		}
	}

	sendOp := reflect.TypeOf((*zlink.SendOp)(nil)).Elem()
	requestOp := reflect.TypeOf((*zlink.RequestOp)(nil)).Elem()
	replyOp := reflect.TypeOf((*zlink.ReplyOp)(nil)).Elem()
	assertReturn((*zlink.PairSocket)(nil), "Send", sendOp)
	assertReturn((*zlink.DealerSocket)(nil), "Send", sendOp)
	assertReturn((*zlink.PubSocket)(nil), "Publish", sendOp)
	assertReturn((*zlink.XPubSocket)(nil), "Publish", sendOp)
	assertReturn((*zlink.RouterSocket)(nil), "SendTo", sendOp)
	assertReturn((*zlink.RouterSocket)(nil), "Request", requestOp)
	assertReturn((*zlink.RouterSocket)(nil), "Reply", replyOp)
	assertReturn((*zlink.StreamSocket)(nil), "SendTo", sendOp)
}

func TestSurfaceRawSocketCapabilities(t *testing.T) {
	checks := []struct {
		target any
		name   string
		want   bool
	}{
		{(*zlink.PairSocket)(nil), "Send", true},
		{(*zlink.PairSocket)(nil), "Recv", true},
		{(*zlink.PairSocket)(nil), "RecvPart", false},
		{(*zlink.PairSocket)(nil), "TrySend", false},
		{(*zlink.PairSocket)(nil), "SendBytes", false},
		{(*zlink.PubSocket)(nil), "Publish", true},
		{(*zlink.PubSocket)(nil), "Recv", false},
		{(*zlink.SubSocket)(nil), "Subscribe", true},
		{(*zlink.SubSocket)(nil), "SubscribePart", false},
		{(*zlink.SubSocket)(nil), "Send", false},
		{(*zlink.RouterSocket)(nil), "SendTo", true},
		{(*zlink.RouterSocket)(nil), "RecvPart", false},
		{(*zlink.RouterSocket)(nil), "OnCompletionControl", true},
		{(*zlink.RouterSocket)(nil), "CompletionControl", true},
		{(*zlink.RouterSocket)(nil), "SetRoutingID", true},
		{(*zlink.RouterSocket)(nil), "SetConnectRoutingID", true},
		{(*zlink.RouterSocket)(nil), "RoutingID", true},
		{(*zlink.RouterSocket)(nil), "Send", false},
		{(*zlink.StreamSocket)(nil), "SetNotify", true},
		{(*zlink.StreamSocket)(nil), "OnPacket", true},
		{(*zlink.StreamSocket)(nil), "Connect", false},
		{(*zlink.StreamSocket)(nil), "Disconnect", false},
		{(*zlink.XPubSocket)(nil), "ReceiveSubscriptionEvent", true},
		{(*zlink.Context)(nil), "SpotNode", false},
	}
	for _, check := range checks {
		if got := hasMethod(check.target, check.name); got != check.want {
			t.Fatalf("%T.%s present=%v, want %v", check.target, check.name, got, check.want)
		}
	}
	if hasMethod((*zlink.Message)(nil), "GetProperty") {
		t.Fatalf("Message should not expose removed property lookup")
	}
	if !hasMethod((*zlink.Message)(nil), "RefCount") {
		t.Fatalf("Message should expose RefCount")
	}
}

func TestSurfaceTypedOptionsAndCallbacks(t *testing.T) {
	checks := []struct {
		target any
		name   string
	}{
		{(*zlink.PairSocket)(nil), "SetSendHighWaterMark"},
		{(*zlink.PairSocket)(nil), "SetTLSServer"},
		{(*zlink.PairSocket)(nil), "SetTLSClient"},
		{(*zlink.PairSocket)(nil), "SetTCPKeepalive"},
		{(*zlink.ContextOptions)(nil), "SetMaxSockets"},
		{(*zlink.ContextOptions)(nil), "Blocky"},
		{(*zlink.ContextOptions)(nil), "SetThreadSchedulingPolicy"},
		{(*zlink.ContextOptions)(nil), "ThreadSchedulingPolicy"},
		{(*zlink.ContextOptions)(nil), "AddThreadAffinity"},
		{(*zlink.ContextOptions)(nil), "RemoveThreadAffinity"},
		{(*zlink.ContextOptions)(nil), "SetAutoHwmMsgUnitBytes"},
		{(*zlink.RouterSocket)(nil), "SetMandatory"},
		{(*zlink.RouterSocket)(nil), "SetRidDuplicatePolicy"},
		{(*zlink.RouterSocket)(nil), "CommonOptions"},
		{(*zlink.DealerSocket)(nil), "CommonOptions"},
		{(*zlink.DealerSocket)(nil), "SetProbe"},
		{(*zlink.PubSocket)(nil), "SetVerboser"},
		{(*zlink.PubSocket)(nil), "SetManual"},
		{(*zlink.SubSocket)(nil), "TopicsCount"},
		{(*zlink.StreamSocket)(nil), "Notify"},
		{(*zlink.StreamSocket)(nil), "SetRoutingID"},
		{(*zlink.StreamSocket)(nil), "RoutingID"},
		{(*zlink.SubSocket)(nil), "SubscriptionAt"},
		{(*zlink.XSubSocket)(nil), "SubscriptionAt"},
		{(*zlink.PairSocket)(nil), "OnSendReady"},
		{(*zlink.DealerSocket)(nil), "OnSendReady"},
		{(*zlink.RouterSocket)(nil), "OnSendReady"},
		{(*zlink.XPubSocket)(nil), "OnSendReady"},
		{(*zlink.StreamSocket)(nil), "OnSendReady"},
	}
	for _, check := range checks {
		if !hasMethod(check.target, check.name) {
			t.Fatalf("%T should expose %s", check.target, check.name)
		}
	}
	if hasMethod((*zlink.PairSocket)(nil), "SetAutoHwmMsgUnitBytes") {
		t.Fatalf("PairSocket should not expose context-level auto-HWM unit option")
	}
	if hasMethod((*zlink.PairSocket)(nil), "SetMandatory") {
		t.Fatalf("PairSocket should not expose router-specific options")
	}
	if hasMethod((*zlink.PubSocket)(nil), "TopicsCount") || hasMethod((*zlink.XPubSocket)(nil), "TopicsCount") {
		t.Fatalf("publish sockets should not expose subscriber-only topic count")
	}
	waitMethod := methodType((*zlink.Poller)(nil), "Wait")
	if waitMethod == nil || waitMethod.NumIn() != 3 || waitMethod.In(1) != reflect.TypeOf([]zlink.PollEvent{}) || waitMethod.In(2) != reflect.TypeOf(time.Duration(0)) {
		t.Fatalf("Poller.Wait signature = %v", waitMethod)
	}
}
