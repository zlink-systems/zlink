package zlink_test

import (
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

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
	_ = snapshot.AutoHwmUnitBudgetBytes
	_ = snapshot.AutoHwmSizeCap
	_ = snapshot.AutoHwmSocketMessageSlots
}

func TestMonitorOnEventReceivesStateChange(t *testing.T) {
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

	events := make(chan *zlink.MonitorEvent, 4)
	if err := serverMon.OnEvent(func(event *zlink.MonitorEvent) {
		events <- event
	}); err != nil {
		t.Fatalf("OnEvent() error = %v", err)
	}
	if _, err := serverMon.Recv(0); err == nil {
		t.Fatalf("Recv() after OnEvent() should fail")
	}

	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := client.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	var event *zlink.MonitorEvent
	select {
	case event = <-events:
	case <-time.After(5 * time.Second):
		t.Fatalf("monitor callback did not receive an event within 5s")
	}

	if !event.IsListening() && !event.IsAccepted() && !event.IsConnectionReady() {
		t.Fatalf("unexpected monitor callback event: %+v", event)
	}
}
