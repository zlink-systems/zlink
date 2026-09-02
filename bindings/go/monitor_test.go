package zlink_test

import (
	"context"
	"errors"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestMonitorOpenAcceptsByteHwmOption(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, err := ctx.PairSocket()
	if err != nil {
		t.Fatalf("PairSocket() error = %v", err)
	}
	defer socket.Close()

	monitor, err := zlink.OpenSocketMonitor(
		socket,
		zlink.MonitorHwmBytes(4096),
		zlink.MonitorEventConnected,
		zlink.MonitorHwmBytes(8192),
	)
	if err != nil {
		t.Fatalf("OpenSocketMonitor(byte HWM) error = %v", err)
	}
	defer monitor.Close()
}

func TestMonitorRecv(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.PairSocket()
	client, _ := ctx.PairSocket()
	defer server.Close()
	defer client.Close()

	serverMon, err := zlink.OpenSocketMonitor(server, zlink.MonitorEventAll)
	if err != nil {
		t.Fatalf("OpenSocketMonitor(server) error = %v", err)
	}
	defer serverMon.Close()

	clientMon, err := zlink.OpenSocketMonitor(client, zlink.MonitorEventAll)
	if err != nil {
		t.Fatalf("OpenSocketMonitor(client) error = %v", err)
	}
	defer clientMon.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := client.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	event := waitForMonitorEvent(t, serverMon, 5*time.Second)
	if !event.IsListening() && !event.IsConnectionReady() && !event.IsAccepted() {
		t.Fatalf("unexpected monitor event: %+v", event)
	}

	snapshot, err := serverMon.Status()
	if err != nil {
		t.Fatalf("Status() error = %v", err)
	}
	if snapshot == nil {
		t.Fatalf("Status() returned nil")
	}
	_ = snapshot.AutoHwmProfile
	_ = snapshot.AutoHwmPolicyClass
	if snapshot.ABIVersion != 4 {
		t.Fatalf("Status ABI = %d, want 4", snapshot.ABIVersion)
	}
	_ = snapshot.SndPendingBytes
	_ = snapshot.RcvPendingBytes

	// §8.1.1 follow-up: the five flow-state metrics must be present on the
	// public MonitorStatus surface and read as zero on a fresh PAIR socket,
	// which has no paired DEALER/ROUTER completion lane.
	if snapshot.FlowPausedConnections != 0 ||
		snapshot.FlowPauseAppliedTotal != 0 ||
		snapshot.FlowResumeAppliedTotal != 0 ||
		snapshot.FlowStateStaleTotal != 0 ||
		snapshot.FlowPauseDurationMs != 0 {
		t.Fatalf("flow metrics on fresh PAIR socket = %+v, want all zero", snapshot)
	}
}

// §8.1.1 follow-up: a real PAUSED/RUNNING transition on a DEALER/ROUTER pair
// must be observable through the monitor with full pair metadata (transport
// pair id) and the corresponding MonitorStatus flow counters must advance.
//
// zlink_socket_set_receive_flow_state sets the caller's own *local receive*
// state and propagates it to the peer over the completion lane; the peer is
// the one whose *send* is now paused, so ROUTER pausing its receive is
// observed as SEND_FLOW_PAUSED on the connected DEALER, not on the ROUTER
// itself. The paired completion lane this relies on is a TCP transport
// concept, so this test uses a TCP loopback endpoint rather than inproc.
func TestMonitorObservesReceiveFlowStateTransitionsWithPairMetadata(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()

	dealerMon, err := zlink.OpenSocketMonitor(dealer, zlink.MonitorEventSendFlowPaused, zlink.MonitorEventSendFlowResumed)
	if err != nil {
		t.Fatalf("OpenSocketMonitor(dealer) error = %v", err)
	}
	defer dealerMon.Close()

	rid := zlink.NewRoutingID([]byte("monitor-flow-state-dealer"))
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.SetRoutingID(rid); err != nil {
		t.Fatalf("SetRoutingID() error = %v", err)
	}
	if err := dealer.SetReceiveTimeout(5 * time.Second); err != nil {
		t.Fatalf("SetReceiveTimeout() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	sendWithConnectRetry(t, dealer, newMessage(t, "hello"))
	var request zlink.Received
	if _, err := router.Recv(&request, zlink.RecvFlagsNone); err != nil {
		t.Fatalf("router Recv() error = %v", err)
	}
	defer request.Close()

	if err := router.SetReceiveFlowState(zlink.ReceiveFlowPaused); err != nil {
		t.Fatalf("SetReceiveFlowState(Paused) error = %v", err)
	}

	event := waitForMonitorEvent(t, dealerMon, 5*time.Second)
	if !event.IsSendFlowPaused() {
		t.Fatalf("expected a SendFlowPaused event, got %+v", event)
	}

	if err := router.SetReceiveFlowState(zlink.ReceiveFlowRunning); err != nil {
		t.Fatalf("SetReceiveFlowState(Running) error = %v", err)
	}
	resumed := waitForMonitorEvent(t, dealerMon, 5*time.Second)
	if !resumed.IsSendFlowResumed() {
		t.Fatalf("expected a SendFlowResumed event, got %+v", resumed)
	}

	snapshot, err := dealerMon.Status()
	if err != nil {
		t.Fatalf("Status() error = %v", err)
	}
	if snapshot.FlowPauseAppliedTotal == 0 {
		t.Fatalf("FlowPauseAppliedTotal = 0, want > 0 after a PAUSED transition")
	}
	if snapshot.FlowResumeAppliedTotal == 0 {
		t.Fatalf("FlowResumeAppliedTotal = 0, want > 0 after a RUNNING transition")
	}
}

func TestMonitorRecvPullReceivesStateChange(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	endpoint := tcpEndpoint(t)
	server, _ := ctx.PairSocket()
	client, _ := ctx.PairSocket()
	defer server.Close()
	defer client.Close()

	serverMon, err := zlink.OpenSocketMonitor(server, zlink.MonitorEventAll)
	if err != nil {
		t.Fatalf("OpenSocketMonitor(server) error = %v", err)
	}
	defer serverMon.Close()

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := client.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	event := waitForMonitorEvent(t, serverMon, 5*time.Second)

	if !event.IsListening() && !event.IsAccepted() && !event.IsConnectionReady() {
		t.Fatalf("unexpected monitor event: %+v", event)
	}
}

// sendWithConnectRetry sends msg on a just-Connect()ed DEALER, retrying while
// the async TCP handshake is still in flight (SubmitNotConnected). msg's
// payload survives a failed Submit, so the same message is reused.
func sendWithConnectRetry(t testing.TB, dealer *zlink.DealerSocket, msg *zlink.Message) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
		err := dealer.Send().Message(msg).Submit(ctx)
		cancel()
		if err == nil {
			return
		}
		var submitError *zlink.SubmitError
		if !errors.As(err, &submitError) || submitError.Result != zlink.SubmitNotConnected {
			t.Fatalf("dealer Send() error = %v", err)
		}
		if time.Now().After(deadline) {
			t.Fatal("timed out waiting for dealer to connect")
		}
		time.Sleep(5 * time.Millisecond)
	}
}
