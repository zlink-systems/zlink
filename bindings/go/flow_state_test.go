// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"bytes"
	"context"
	"errors"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

// §8.1.1 "Flow-state enum 값이 C ABI 값과 같다": ZLINK_RECEIVE_FLOW_RUNNING = 0,
// ZLINK_RECEIVE_FLOW_PAUSED = 1 (core/include/zlink_enum.h).
func TestReceiveFlowStateEnumMatchesCABI(t *testing.T) {
	if zlink.ReceiveFlowRunning != zlink.ReceiveFlowState(0) {
		t.Fatalf("ReceiveFlowRunning = %d, want 0", zlink.ReceiveFlowRunning)
	}
	if zlink.ReceiveFlowPaused != zlink.ReceiveFlowState(1) {
		t.Fatalf("ReceiveFlowPaused = %d, want 1", zlink.ReceiveFlowPaused)
	}
}

// Final-review follow-up (plan §6): the three flow-state monitor event
// constants and the event-flag bits Core reuses on zlink_monitor_event_t.flags
// must be on the public root-package surface with values matching the C ABI
// (core/include/zlink_enum.h, core/include/zlink/eventing/api.h), and
// MonitorEventAll must still cover them.
func TestFlowStateMonitorEventConstantsMatchCABIAndArePublic(t *testing.T) {
	maskCases := []struct {
		name string
		got  zlink.MonitorEventMask
		want zlink.MonitorEventMask
	}{
		{"MonitorEventSendFlowPaused", zlink.MonitorEventSendFlowPaused, 1 << 16},
		{"MonitorEventSendFlowResumed", zlink.MonitorEventSendFlowResumed, 1 << 17},
		{"MonitorEventFlowStateStale", zlink.MonitorEventFlowStateStale, 1 << 18},
	}
	for _, tc := range maskCases {
		if tc.got != tc.want {
			t.Fatalf("%s = %#x, want %#x", tc.name, tc.got, tc.want)
		}
		if zlink.MonitorEventAll&tc.got == 0 {
			t.Fatalf("MonitorEventAll does not cover %s", tc.name)
		}
	}
	if zlink.MonitorEventAll != zlink.MonitorEventMask(0x7FFFF) {
		t.Fatalf("MonitorEventAll = %#x, want %#x", zlink.MonitorEventAll, 0x7FFFF)
	}

	typeCases := []struct {
		name string
		got  zlink.MonitorEventType
		want zlink.MonitorEventType
	}{
		{"MonitorEventTypeSendFlowPaused", zlink.MonitorEventTypeSendFlowPaused, 1 << 16},
		{"MonitorEventTypeSendFlowResumed", zlink.MonitorEventTypeSendFlowResumed, 1 << 17},
		{"MonitorEventTypeFlowStateStale", zlink.MonitorEventTypeFlowStateStale, 1 << 18},
	}
	for _, tc := range typeCases {
		if tc.got != tc.want {
			t.Fatalf("%s = %#x, want %#x", tc.name, tc.got, tc.want)
		}
	}

	flagCases := []struct {
		name string
		got  zlink.MonitorEventFlag
		want zlink.MonitorEventFlag
	}{
		{"MonitorEventFlagConnectionReadyEdge", zlink.MonitorEventFlagConnectionReadyEdge, 1 << 0},
		{"MonitorEventFlagSendFlowWritable", zlink.MonitorEventFlagSendFlowWritable, 1 << 1},
		{"MonitorEventFlagFlowStateStaleGeneration", zlink.MonitorEventFlagFlowStateStaleGeneration, 1 << 2},
		{"MonitorEventFlagFlowStateStaleEpoch", zlink.MonitorEventFlagFlowStateStaleEpoch, 1 << 3},
	}
	for _, tc := range flagCases {
		if tc.got != tc.want {
			t.Fatalf("%s = %#x, want %#x", tc.name, tc.got, tc.want)
		}
	}

	if zlink.MonitorTransportLaneApplication != 0 || zlink.MonitorTransportLaneCompletion != 1 {
		t.Fatalf("MonitorTransportLane values = (%d, %d), want (0, 1)",
			zlink.MonitorTransportLaneApplication, zlink.MonitorTransportLaneCompletion)
	}
}

// Final-review follow-up: the five flow-state fields must be on the public
// MonitorStatus surface (checked by field existence via the compiler; a
// runtime zero-check on a fresh PAIR socket lives in monitor_test.go).
func TestMonitorStatusExposesFlowMetricFields(t *testing.T) {
	var status zlink.MonitorStatus
	status.FlowPausedConnections = 1
	status.FlowPauseAppliedTotal = 2
	status.FlowResumeAppliedTotal = 3
	status.FlowStateStaleTotal = 4
	status.FlowPauseDurationMs = 5
	if status.FlowPausedConnections != 1 || status.FlowPauseAppliedTotal != 2 ||
		status.FlowResumeAppliedTotal != 3 || status.FlowStateStaleTotal != 4 ||
		status.FlowPauseDurationMs != 5 {
		t.Fatalf("MonitorStatus flow metric fields did not round-trip: %+v", status)
	}
}

// Spec-round follow-up: ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE
// (core/include/zlink_enum.h) must be reachable via IsFlowStateDetailPopulated()
// on the public MonitorStatus surface, agreeing bit-for-bit with DetailFlags.
func TestMonitorStatusDetailFlowStateBitMatchesCABI(t *testing.T) {
	var status zlink.MonitorStatus
	status.DetailFlags = 1 << 5
	if !status.IsFlowStateDetailPopulated() {
		t.Fatalf("IsFlowStateDetailPopulated() = false with DetailFlags = 1<<5, want true")
	}
	status.DetailFlags = 0
	if status.IsFlowStateDetailPopulated() {
		t.Fatalf("IsFlowStateDetailPopulated() = true with DetailFlags = 0, want false")
	}
}

// §8.1.1 "DEALER/ROUTER socket에서 설정이 성공하고 같은 state의 반복 호출도 성공한다".
func TestSetReceiveFlowStateSucceedsOnDealerAndRouterAndIsIdempotent(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	dealer, _ := ctx.DealerSocket()
	defer dealer.Close()
	router, _ := ctx.RouterSocket()
	defer router.Close()

	for name, socket := range map[string]interface {
		SetReceiveFlowState(zlink.ReceiveFlowState) error
	}{
		"dealer": dealer,
		"router": router,
	} {
		if err := socket.SetReceiveFlowState(zlink.ReceiveFlowPaused); err != nil {
			t.Fatalf("%s SetReceiveFlowState(Paused) error = %v", name, err)
		}
		// Idempotent repeat of the same state must also succeed.
		if err := socket.SetReceiveFlowState(zlink.ReceiveFlowPaused); err != nil {
			t.Fatalf("%s SetReceiveFlowState(Paused) repeat error = %v", name, err)
		}
		if err := socket.SetReceiveFlowState(zlink.ReceiveFlowRunning); err != nil {
			t.Fatalf("%s SetReceiveFlowState(Running) error = %v", name, err)
		}
		if err := socket.SetReceiveFlowState(zlink.ReceiveFlowRunning); err != nil {
			t.Fatalf("%s SetReceiveFlowState(Running) repeat error = %v", name, err)
		}
	}
}

// §8.1.1 "PAIR, PUB/SUB 계열과 STREAM에서 not-supported 오류 mapping을 반환한다".
func TestSetReceiveFlowStateReportsNotSupportedOnUnsupportedSocketTypes(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	pair, _ := ctx.PairSocket()
	defer pair.Close()
	pub, _ := ctx.PubSocket()
	defer pub.Close()
	sub, _ := ctx.SubSocket()
	defer sub.Close()
	xpub, _ := ctx.XPubSocket()
	defer xpub.Close()
	xsub, _ := ctx.XSubSocket()
	defer xsub.Close()
	stream, _ := ctx.StreamSocket()
	defer stream.Close()

	cases := map[string]interface {
		SetReceiveFlowState(zlink.ReceiveFlowState) error
	}{
		"pair":   pair,
		"pub":    pub,
		"sub":    sub,
		"xpub":   xpub,
		"xsub":   xsub,
		"stream": stream,
	}
	for name, socket := range cases {
		err := socket.SetReceiveFlowState(zlink.ReceiveFlowPaused)
		var configErr *zlink.ConfigError
		if !errors.As(err, &configErr) || configErr.Result != zlink.ConfigNotSupported {
			t.Fatalf("%s SetReceiveFlowState() error = %v, want ConfigError{Result: ConfigNotSupported}", name, err)
		}
	}
}

// §8.1.1 "Invalid handle·argument·state가 언어 오류 정책대로 mapping된다": handle case.
func TestSetReceiveFlowStateOnClosedSocketIsInvalidHandle(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	dealer, _ := ctx.DealerSocket()
	if err := dealer.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}

	err := dealer.SetReceiveFlowState(zlink.ReceiveFlowPaused)
	var configErr *zlink.ConfigError
	if !errors.As(err, &configErr) || configErr.Result != zlink.ConfigInvalidHandle {
		t.Fatalf("SetReceiveFlowState() on closed socket error = %v, want ConfigError{Result: ConfigInvalidHandle}", err)
	}
}

// §8.1.1 "Invalid handle·argument·state가 언어 오류 정책대로 mapping된다": argument case.
func TestSetReceiveFlowStateOutOfRangeIsInvalidArgument(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	dealer, _ := ctx.DealerSocket()
	defer dealer.Close()

	for _, value := range []zlink.ReceiveFlowState{2, -1, 999} {
		err := dealer.SetReceiveFlowState(value)
		var configErr *zlink.ConfigError
		if !errors.As(err, &configErr) || configErr.Result != zlink.ConfigInvalidArgument {
			t.Fatalf("SetReceiveFlowState(%d) error = %v, want ConfigError{Result: ConfigInvalidArgument}", value, err)
		}
	}
}

// §8.1.1 "Close와 경쟁해도 성공한 설정이나 close 오류 중 하나만 관찰된다".
func TestSetReceiveFlowStateRacingCloseObservesOnlyOkOrCloseRelatedError(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	for i := 0; i < 20; i++ {
		dealer, _ := ctx.DealerSocket()

		var wg sync.WaitGroup
		wg.Add(2)
		var setErr error
		go func() {
			defer wg.Done()
			setErr = dealer.SetReceiveFlowState(zlink.ReceiveFlowPaused)
		}()
		go func() {
			defer wg.Done()
			_ = dealer.Close()
		}()
		wg.Wait()

		if setErr == nil {
			continue
		}
		var configErr *zlink.ConfigError
		if !errors.As(setErr, &configErr) {
			t.Fatalf("iteration %d: SetReceiveFlowState() during close error = %v, want nil or *ConfigError", i, setErr)
		}
		if configErr.Result != zlink.ConfigInvalidState && configErr.Result != zlink.ConfigInvalidHandle {
			t.Fatalf("iteration %d: SetReceiveFlowState() during close Result = %v, want InvalidState or InvalidHandle", i, configErr.Result)
		}
	}
}

// §8.1.1 "공개 표면 test에 flow frame receive·encode API가 없다": the only flow-state
// surface is the local state setter — no flow-frame receive/encode/pause-bypass API.
func TestPublicSurfaceHasNoFlowFrameAPI(t *testing.T) {
	forbidden := []string{"flowframe", "encodeflow", "receiveflowframe", "sendflowframe", "pausebypass"}

	socketTypes := []any{
		(*zlink.PairSocket)(nil),
		(*zlink.PubSocket)(nil),
		(*zlink.SubSocket)(nil),
		(*zlink.DealerSocket)(nil),
		(*zlink.RouterSocket)(nil),
		(*zlink.XPubSocket)(nil),
		(*zlink.XSubSocket)(nil),
		(*zlink.StreamSocket)(nil),
		(*zlink.CommonSocketOptions)(nil),
	}

	flowMethodsSeen := map[string]bool{}
	for _, target := range socketTypes {
		typ := reflect.TypeOf(target)
		for i := 0; i < typ.NumMethod(); i++ {
			name := typ.Method(i).Name
			lower := strings.ToLower(name)
			if strings.Contains(lower, "flow") {
				flowMethodsSeen[name] = true
			}
			for _, bad := range forbidden {
				if strings.Contains(lower, bad) {
					t.Fatalf("%v exposes forbidden flow-frame API %s", typ, name)
				}
			}
		}
	}

	want := map[string]bool{"SetReceiveFlowState": true}
	if !reflect.DeepEqual(flowMethodsSeen, want) {
		t.Fatalf("flow-related public methods = %v, want %v", flowMethodsSeen, want)
	}
}

// §8.1.1 "기존 HWM, EAGAIN 상당 결과와 send timeout 동작이 변하지 않는다": smoke that
// ordinary HWM configuration and DEALER/ROUTER traffic are unaffected by the new setter.
func TestExistingHWMBehaviorUnchangedAfterReceiveFlowStateCalls(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := inprocEndpoint("flow-state-hwm-smoke")
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()

	if err := dealer.SetSendHighWaterMark(1000); err != nil {
		t.Fatalf("SetSendHighWaterMark() error = %v", err)
	}
	if got, err := dealer.SendHighWaterMark(); err != nil || got != 1000 {
		t.Fatalf("SendHighWaterMark() = (%d, %v), want (1000, nil)", got, err)
	}

	// Setting the receive-flow state (including a repeat of the default RUNNING
	// state) must not perturb byte-HWM configuration or ordinary traffic.
	if err := router.SetReceiveFlowState(zlink.ReceiveFlowRunning); err != nil {
		t.Fatalf("router SetReceiveFlowState(Running) error = %v", err)
	}
	if err := dealer.SetReceiveFlowState(zlink.ReceiveFlowRunning); err != nil {
		t.Fatalf("dealer SetReceiveFlowState(Running) error = %v", err)
	}

	if got, err := dealer.SendHighWaterMark(); err != nil || got != 1000 {
		t.Fatalf("SendHighWaterMark() after SetReceiveFlowState = (%d, %v), want (1000, nil)", got, err)
	}

	rid := zlink.NewRoutingID([]byte("flow-state-hwm-smoke-dealer"))
	_ = router.Bind(endpoint)
	_ = dealer.SetRoutingID(rid)
	_ = dealer.Connect(endpoint)
	_ = dealer.SetReceiveTimeout(5 * time.Second)

	if err := awaitRoutedSend(t, dealer.Send().Message(newMessage(t, "request")).Submit(context.Background())); err != nil {
		t.Fatalf("dealer Send() error = %v", err)
	}

	var request zlink.Received
	if _, err := router.Recv(&request, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("router Recv() error = %v", err)
	}
	defer request.Close()

	if err := awaitRoutedSend(t, router.SendTo(request.RoutingID()).Message(newMessage(t, "response")).Submit(context.Background())); err != nil {
		t.Fatalf("router SendTo() error = %v", err)
	}

	var response zlink.Received
	if _, err := dealer.Recv(&response, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("dealer Recv() error = %v", err)
	}
	defer response.Close()

	part, _ := response.SinglePartOrError()
	if !bytes.Equal(part.Data(), []byte("response")) {
		t.Fatalf("unexpected response = %q", string(part.Data()))
	}
}
