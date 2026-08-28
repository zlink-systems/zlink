package zlink_test

import (
	"context"
	"reflect"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
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
	routedSendOp := reflect.TypeOf((*zlink.RoutedSendOp)(nil)).Elem()
	requestOp := reflect.TypeOf((*zlink.RequestOp)(nil)).Elem()
	replyOp := reflect.TypeOf((*zlink.ReplyOp)(nil)).Elem()
	assertReturn((*zlink.PairSocket)(nil), "Send", sendOp)
	assertReturn((*zlink.DealerSocket)(nil), "Send", routedSendOp)
	assertReturn((*zlink.DealerSocket)(nil), "Request", requestOp)
	assertReturn((*zlink.PubSocket)(nil), "Publish", sendOp)
	assertReturn((*zlink.XPubSocket)(nil), "Publish", sendOp)
	assertReturn((*zlink.RouterSocket)(nil), "SendTo", routedSendOp)
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
		{(*zlink.ContextOptions)(nil), "SetCoreHwmBudgetBytes"},
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
	}
	for _, check := range checks {
		if !hasMethod(check.target, check.name) {
			t.Fatalf("%T should expose %s", check.target, check.name)
		}
	}
	if hasMethod((*zlink.PairSocket)(nil), "SetMandatory") {
		t.Fatalf("PairSocket should not expose router-specific options")
	}
	// Core 0.13.1 has no send_ready readiness hint: send completion is
	// Core-owned, so no socket may expose a readiness callback.
	for _, target := range []any{
		(*zlink.PairSocket)(nil),
		(*zlink.PubSocket)(nil),
		(*zlink.XPubSocket)(nil),
		(*zlink.StreamSocket)(nil),
		(*zlink.DealerSocket)(nil),
		(*zlink.RouterSocket)(nil),
	} {
		if hasMethod(target, "OnSendReady") {
			t.Fatalf("%T should not expose the retired send_ready readiness callback", target)
		}
	}
	if hasMethod((*zlink.PubSocket)(nil), "TopicsCount") || hasMethod((*zlink.XPubSocket)(nil), "TopicsCount") {
		t.Fatalf("publish sockets should not expose subscriber-only topic count")
	}
	waitMethod := methodType((*zlink.Poller)(nil), "Wait")
	if waitMethod == nil || waitMethod.NumIn() != 3 || waitMethod.In(1) != reflect.TypeOf([]zlink.PollEvent{}) || waitMethod.In(2) != reflect.TypeOf(time.Duration(0)) {
		t.Fatalf("Poller.Wait signature = %v", waitMethod)
	}
}

func TestSurfaceManagedRoutedTerminalHasSyncSubmitAndSendFlags(t *testing.T) {
	contextType := reflect.TypeOf((*context.Context)(nil)).Elem()
	// Routed send is synchronous: Submit(ctx) error plus builder flags. Request
	// keeps its async completion-channel terminal and narrows through Flags to
	// the synchronous admission terminal.
	sendCompletionType := reflect.TypeOf((*error)(nil)).Elem()
	requestCompletionType := reflect.TypeOf((<-chan zlink.RequestReplyCompletion)(nil))
	sendFlagsType := reflect.TypeOf(zlink.SendFlagsNone)

	assertTerminal := func(target reflect.Type, completion reflect.Type) {
		t.Helper()
		method, ok := target.MethodByName("Submit")
		if !ok || method.Type.NumIn() != 1 || method.Type.In(0) != contextType || method.Type.NumOut() != 1 || method.Type.Out(0) != completion {
			t.Fatalf("%v.Submit signature = %v", target, method.Type)
		}
		for _, forbidden := range []string{"SubmitAsync", "Callback", "OnProgress"} {
			if _, ok := target.MethodByName(forbidden); ok {
				t.Fatalf("%v should not expose compatibility terminal %s", target, forbidden)
			}
		}
	}

	routedType := reflect.TypeOf((*zlink.RoutedSendSubmitOp)(nil)).Elem()
	assertTerminal(routedType, sendCompletionType)
	flagsMethod, ok := routedType.MethodByName("Flags")
	if !ok || flagsMethod.Type.NumIn() != 1 || flagsMethod.Type.In(0) != sendFlagsType || flagsMethod.Type.NumOut() != 1 || flagsMethod.Type.Out(0) != routedType {
		t.Fatalf("%v.Flags signature = %v", routedType, flagsMethod.Type)
	}

	requestType := reflect.TypeOf((*zlink.RequestSubmitOp)(nil)).Elem()
	assertTerminal(requestType, requestCompletionType)
	requestSyncType := reflect.TypeOf((*zlink.RequestSyncSubmitOp)(nil)).Elem()
	flagsMethod, ok = requestType.MethodByName("Flags")
	if !ok || flagsMethod.Type.NumIn() != 1 || flagsMethod.Type.In(0) != sendFlagsType || flagsMethod.Type.NumOut() != 1 || flagsMethod.Type.Out(0) != requestSyncType {
		t.Fatalf("%v.Flags signature = %v", requestType, flagsMethod.Type)
	}
	syncSubmit, ok := requestSyncType.MethodByName("Submit")
	if !ok || syncSubmit.Type.NumIn() != 1 || syncSubmit.Type.In(0) != contextType || syncSubmit.Type.NumOut() != 2 || syncSubmit.Type.Out(0) != requestCompletionType || syncSubmit.Type.Out(1) != sendCompletionType {
		t.Fatalf("%v.Submit signature = %v", requestSyncType, syncSubmit.Type)
	}
}

func TestSurfaceMonitorOpenUsesOnlyByteHwmOption(t *testing.T) {
	optionType := reflect.TypeOf((*zlink.MonitorOpenOption)(nil)).Elem()
	openType := reflect.TypeOf(zlink.OpenSocketMonitor)
	if !openType.IsVariadic() || openType.NumIn() != 2 || openType.In(1) != reflect.TypeOf([]zlink.MonitorOpenOption(nil)) {
		t.Fatalf("OpenSocketMonitor signature = %v, want variadic MonitorOpenOption", openType)
	}
	hwmType := reflect.TypeOf(zlink.MonitorHwmBytes)
	if hwmType.NumIn() != 1 || hwmType.In(0).Kind() != reflect.Uint64 || hwmType.NumOut() != 1 || hwmType.Out(0) != optionType {
		t.Fatalf("MonitorHwmBytes signature = %v, want func(uint64) MonitorOpenOption", hwmType)
	}
	rootNames := exportedTopLevelNames(t, ".")
	for _, legacy := range []string{"MonitorHwm", "MonitorHwmCount", "MonitorHwmMessages"} {
		if _, ok := rootNames[legacy]; ok {
			t.Fatalf("root package should not expose legacy monitor count option %s", legacy)
		}
	}
}
