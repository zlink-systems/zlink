package native

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"syscall"
	"testing"
)

func TestRoutingIDCanonicalHelpers(t *testing.T) {
	rid := NewRoutingID([]byte{0x00, 0x41, 0x42})
	if rid.Size() != 3 {
		t.Fatalf("Size() = %d, want 3", rid.Size())
	}
	if got := rid.Hex(); got != "004142" {
		t.Fatalf("Hex() = %q, want %q", got, "004142")
	}
	if got := rid.String(); got != "hex:004142" {
		t.Fatalf("String() = %q, want %q", got, "hex:004142")
	}
	if got := NewRoutingIDFromHex("004142"); !got.Equal(rid) {
		t.Fatalf("NewRoutingIDFromHex() = %v, want %v", got, rid)
	}
	parsed, err := parseRoutingIDHex("004142")
	if err != nil {
		t.Fatalf("parseRoutingIDHex() error = %v", err)
	}
	if !parsed.Equal(rid) {
		t.Fatalf("parseRoutingIDHex() = %v, want %v", parsed, rid)
	}
	maxParsed, err := parseRoutingIDHex(strings.Repeat("a", 510))
	if err != nil {
		t.Fatalf("parseRoutingIDHex(max) error = %v", err)
	}
	if maxParsed.Size() != 255 {
		t.Fatalf("parseRoutingIDHex(max).Size() = %d, want 255", maxParsed.Size())
	}
	if got := NewRoutingIDFromHex("not-hex"); got.Size() != 0 {
		t.Fatalf("NewRoutingIDFromHex(invalid).Size() = %d, want 0", got.Size())
	}
	if got := NewRoutingIDFromHex(strings.Repeat("a", 512)); got.Size() != 0 {
		t.Fatalf("NewRoutingIDFromHex(oversize).Size() = %d, want 0", got.Size())
	}
	if _, err := parseRoutingIDHex(strings.Repeat("a", 512)); err == nil {
		t.Fatalf("parseRoutingIDHex(oversize) should fail")
	} else {
		var configErr *ConfigError
		if !errors.As(err, &configErr) {
			t.Fatalf("parseRoutingIDHex(oversize) error type = %T, want *ConfigError", err)
		}
	}
	if got := NewRoutingIDString("dealer-1").String(); got != "dealer-1" {
		t.Fatalf("NewRoutingIDString().String() = %q, want dealer-1", got)
	}
	if got := NewRoutingIDUint32(23).String(); got != "23" {
		t.Fatalf("NewRoutingIDUint32().String() = %q, want 23", got)
	}
	if rid.Hash() != NewRoutingID([]byte{0x00, 0x41, 0x42}).Hash() {
		t.Fatalf("Hash() should be stable for equal routing ids")
	}
	bytes := rid.Bytes()
	bytes[0] = 0xFF
	if got := rid.Bytes()[0]; got != 0x00 {
		t.Fatalf("Bytes() should return a defensive copy, got %#x", got)
	}
}

func TestReceivedAndTopicConvenienceHelpersUseRecvError(t *testing.T) {
	recv := &Received{}
	if _, err := recv.FirstPart(); err == nil {
		t.Fatalf("FirstPart() should fail on empty recv")
	} else {
		var recvErr *RecvError
		if !errors.As(err, &recvErr) {
			t.Fatalf("FirstPart() error type = %T, want *RecvError", err)
		}
	}
	if _, err := recv.SinglePartOrError(); err == nil {
		t.Fatalf("SinglePartOrError() should fail on empty recv")
	} else {
		var recvErr *RecvError
		if !errors.As(err, &recvErr) {
			t.Fatalf("SinglePartOrError() error type = %T, want *RecvError", err)
		}
	}

	topic := &TopicMessage{}
	if _, err := topic.FirstPart(); err == nil {
		t.Fatalf("TopicMessage.FirstPart() should fail on empty topic message")
	} else {
		var recvErr *RecvError
		if !errors.As(err, &recvErr) {
			t.Fatalf("TopicMessage.FirstPart() error type = %T, want *RecvError", err)
		}
	}
	if _, err := topic.SinglePartOrError(); err == nil {
		t.Fatalf("TopicMessage.SinglePartOrError() should fail on empty topic message")
	} else {
		var recvErr *RecvError
		if !errors.As(err, &recvErr) {
			t.Fatalf("TopicMessage.SinglePartOrError() error type = %T, want *RecvError", err)
		}
	}
}

func TestRequestCompletionErrorUsesStableErrno(t *testing.T) {
	cases := []struct {
		name   string
		result RequestResult
		errno  syscall.Errno
	}{
		{name: "timeout", result: RequestTimedOut, errno: syscall.ETIMEDOUT},
		{name: "not-connected", result: RequestNotConnected, errno: syscall.ENOTCONN},
		{name: "backpressured", result: RequestBackpressured, errno: syscall.EAGAIN},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := requestCompletionError(tc.result)
			var requestErr *RequestError
			if !errors.As(err, &requestErr) {
				t.Fatalf("error type = %T, want *RequestError", err)
			}
			if got := requestErr.InternalErrno(); got != int(tc.errno) {
				t.Fatalf("InternalErrno() = %d, want %d", got, tc.errno)
			}
			if !errors.Is(err, tc.errno) {
				t.Fatalf("errors.Is(%v) = false", tc.errno)
			}
		})
	}
}

func TestCoreResultCodeValuesRemainComplete(t *testing.T) {
	cases := map[string]struct {
		got  int
		want int
	}{
		"request backpressured":   {got: int(RequestBackpressured), want: 113},
		"recv buffer too small":   {got: int(RecvBufferTooSmall), want: 207},
		"recv invalid state":      {got: int(RecvInvalidState), want: 208},
		"connect auth failed":     {got: int(ConnectAuthFailed), want: 608},
		"config conflict":         {got: int(ConfigConflict), want: 707},
		"config buffer too small": {got: int(ConfigBufferTooSmall), want: 708},
		"config busy":             {got: int(ConfigBusy), want: 709},
	}
	for name, tc := range cases {
		if tc.got != tc.want {
			t.Errorf("%s = %d, want %d", name, tc.got, tc.want)
		}
	}
}

func TestCoreEventFlagValuesRemainComplete(t *testing.T) {
	monitorMasks := []struct {
		name string
		got  MonitorEventMask
		want MonitorEventMask
	}{
		{"connected", MonitorEventConnected, 1 << 0},
		{"connect delayed", MonitorEventConnectDelayed, 1 << 1},
		{"connect retried", MonitorEventConnectRetried, 1 << 2},
		{"listening", MonitorEventListening, 1 << 3},
		{"bind failed", MonitorEventBindFailed, 1 << 4},
		{"accepted", MonitorEventAccepted, 1 << 5},
		{"accept failed", MonitorEventAcceptFailed, 1 << 6},
		{"closed", MonitorEventClosed, 1 << 7},
		{"close failed", MonitorEventCloseFailed, 1 << 8},
		{"disconnected", MonitorEventDisconnected, 1 << 9},
		{"monitor stopped", MonitorEventMonitorStopped, 1 << 10},
		{"handshake failed no detail", MonitorEventHandshakeFailedNoDetail, 1 << 11},
		{"connection ready", MonitorEventConnectionReady, 1 << 12},
		{"handshake failed protocol", MonitorEventHandshakeFailedProtocol, 1 << 13},
		{"handshake failed auth", MonitorEventHandshakeFailedAuth, 1 << 14},
		{"peer weight changed", MonitorEventPeerWeightChanged, 1 << 15},
		{"all", MonitorEventAll, 0xFFFF},
	}
	for _, tc := range monitorMasks {
		if tc.got != tc.want {
			t.Errorf("MonitorEvent%s = %#x, want %#x", tc.name, tc.got, tc.want)
		}
	}
	if MonitorEventTypeConnectionReady != MonitorEventType(MonitorEventConnectionReady) {
		t.Fatalf("MonitorEventTypeConnectionReady = %#x, want %#x", MonitorEventTypeConnectionReady, MonitorEventConnectionReady)
	}
	if PollIn != 1 || PollOut != 2 || PollErr != 4 || PollPri != 8 || PollCompletion != 32 {
		t.Fatalf("poll flags = (%d, %d, %d, %d, %d), want (1, 2, 4, 8, 32)", PollIn, PollOut, PollErr, PollPri, PollCompletion)
	}
}

func TestReceivedReplyHelpersCarryCanonicalMetadata(t *testing.T) {
	replyPart, err := NewMessage([]byte("reply"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer replyPart.Close()

	var gotFlags SendFlags
	var gotRequestSeq uint64
	var gotReplyCount int
	received := &Received{
		routingID:     NewRoutingID([]byte("peer")),
		requestSeq:    42,
		hasRequestSeq: true,
		parts:         []*Message{replyPart},
	}
	received.reply = func(flags SendFlags, parts []*Message) error {
		gotFlags = flags
		gotRequestSeq = received.RequestSeq()
		gotReplyCount = len(parts)
		return nil
	}

	if !received.HasRoutingID() || !received.HasRequestSeq() {
		t.Fatalf("received helper predicates should reflect stored metadata")
	}
	if !received.IsSinglePart() {
		t.Fatalf("IsSinglePart() = false, want true")
	}
	if err := received.Reply().Message(replyPart).Submit(context.Background()); err != nil {
		t.Fatalf("Reply() error = %v", err)
	}
	if gotFlags != SendFlagsNone {
		t.Fatalf("Reply() passed flags %v, want %v", gotFlags, SendFlagsNone)
	}
	if gotRequestSeq != 42 {
		t.Fatalf("RequestSeq() = %d, want 42", gotRequestSeq)
	}
	if gotReplyCount != 1 {
		t.Fatalf("reply part count = %d, want 1", gotReplyCount)
	}
}

func TestReceivedReplyBuilderRejectsUnsupportedFlags(t *testing.T) {
	replyPart, err := NewMessage([]byte("reply"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer replyPart.Close()

	called := false
	received := &Received{
		requestSeq:    42,
		hasRequestSeq: true,
		reply: func(flags SendFlags, parts []*Message) error {
			called = true
			return nil
		},
	}

	err = received.Reply().Message(replyPart).Flags(SendFlagsDontWait).Submit(context.Background())
	if err == nil {
		t.Fatalf("Reply() should reject unsupported flags")
	}
	var submitErr *SubmitError
	if !errors.As(err, &submitErr) {
		t.Fatalf("Reply() error type = %T, want *SubmitError", err)
	}
	if submitErr.Result != SubmitNotSupported {
		t.Fatalf("Reply() result = %v, want %v", submitErr.Result, SubmitNotSupported)
	}
	if called {
		t.Fatalf("Reply() should not invoke the reply callback for unsupported flags")
	}
}

func TestReceivedReplyRequiresValidContext(t *testing.T) {
	received := &Received{}
	msg, err := NewMessage([]byte("reply"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer msg.Close()
	err = received.Reply().Message(msg).Submit(context.Background())
	if err == nil {
		t.Fatalf("Reply() should fail without a request context")
	}
	var submitErr *SubmitError
	if !errors.As(err, &submitErr) {
		t.Fatalf("Reply() error type = %T, want *SubmitError", err)
	}
	if submitErr.Result != SubmitInvalidArgument {
		t.Fatalf("Reply() result = %v, want %v", submitErr.Result, SubmitInvalidArgument)
	}
}

func TestExportedSpecShapeForMonitorDiscoveryAndErrors(t *testing.T) {
	assertField := func(name string, typ reflect.Type, kind reflect.Kind) {
		field, ok := typ.FieldByName(name)
		if !ok {
			t.Fatalf("%s should expose field %s", typ.Name(), name)
		}
		if kind != reflect.Invalid && field.Type.Kind() != kind {
			t.Fatalf("%s.%s kind = %v, want %v", typ.Name(), name, field.Type.Kind(), kind)
		}
	}
	assertNoField := func(name string, typ reflect.Type) {
		if _, ok := typ.FieldByName(name); ok {
			t.Fatalf("%s should not expose legacy field %s", typ.Name(), name)
		}
	}

	assertField("SourceKind", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("SndPendingMsgs", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("RcvPendingMsgs", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmProfile", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmPolicyClass", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmUnitBudgetBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmSizeCap", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmSocketMessageSlots", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmConnectionBucketEnabled", reflect.TypeOf(MonitorStatus{}), reflect.Bool)
	assertField("AutoHwmConnectionBucketCount", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmConnectionBucketIndex", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmConnectionBucketHwm4K", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmConnectionBucketHysteresisRetained", reflect.TypeOf(MonitorStatus{}), reflect.Bool)
	assertField("AutoHwmLastRecalcReason", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmSendBlockedRatioPPM", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("ABIVersion", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("StructSize", reflect.TypeOf(MonitorStatus{}), reflect.Uint32)
	assertField("AutoHwmPlannedSndHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmPlannedRcvHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmAppliedSndHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmAppliedRcvHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmDeferredSndHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmDeferredRcvHwmBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("AutoHwmDeferredSndHwmValid", reflect.TypeOf(MonitorStatus{}), reflect.Bool)
	assertField("AutoHwmDeferredRcvHwmValid", reflect.TypeOf(MonitorStatus{}), reflect.Bool)
	assertField("SndBytesInFlight", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("RcvBytesInFlight", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("MinimumCoreMessageChargeBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("OversizeMessageAdmissionCount", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertField("OversizeMessageAdmissionMaxBytes", reflect.TypeOf(MonitorStatus{}), reflect.Uint64)
	assertNoField("AutoHwmObservedCount", reflect.TypeOf(MonitorStatus{}))
	assertNoField("AutoHwmEffectivePublishFanout", reflect.TypeOf(MonitorStatus{}))
	assertNoField("AutoHwmScope", reflect.TypeOf(MonitorStatus{}))
	assertNoField("AutoHwmControlBudgetBytes", reflect.TypeOf(MonitorStatus{}))
	assertNoField("AutoHwmControlActiveConnections", reflect.TypeOf(MonitorStatus{}))
	assertNoField("SendPendingMsg", reflect.TypeOf(MonitorStatus{}))
	assertNoField("RecvPendingMsg", reflect.TypeOf(MonitorStatus{}))

	methodNames := []struct {
		typ  reflect.Type
		name string
	}{
		{reflect.TypeOf(&MonitorEvent{}), "HasRoutingID"},
	}
	for _, method := range methodNames {
		if _, ok := method.typ.MethodByName(method.name); !ok {
			t.Fatalf("%s should expose %s()", method.typ, method.name)
		}
	}

	errorTypes := []reflect.Type{
		reflect.TypeOf(SubmitError{}),
		reflect.TypeOf(RequestError{}),
		reflect.TypeOf(RecvError{}),
		reflect.TypeOf(HandlerError{}),
		reflect.TypeOf(CloseError{}),
		reflect.TypeOf(BindError{}),
		reflect.TypeOf(ConnectError{}),
		reflect.TypeOf(ConfigError{}),
	}
	for _, typ := range errorTypes {
		if _, ok := typ.FieldByName("NativeErrno"); ok {
			t.Fatalf("%s should not expose NativeErrno field", typ.Name())
		}
		if _, ok := reflect.PointerTo(typ).MethodByName("InternalErrno"); !ok {
			t.Fatalf("%s should expose InternalErrno()", typ.Name())
		}
	}
}

func TestSubscriptionEventHasRoutingID(t *testing.T) {
	event := SubscriptionEvent{
		routingID:  NewRoutingID([]byte("subscriber")),
		subscribed: true,
		topic:      "topic.alpha",
	}
	if !event.HasRoutingID() {
		t.Fatalf("HasRoutingID() = false, want true")
	}
	if got := event.Topic(); got != "topic.alpha" {
		t.Fatalf("Topic() = %q, want %q", got, "topic.alpha")
	}
	if !event.Subscribed() {
		t.Fatalf("Subscribed() = false, want true")
	}
}
