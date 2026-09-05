// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"errors"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestMonitorPollerLifecycle(t *testing.T) {
	for _, transport := range []string{"inproc", "tcp"} {
		for _, alias := range []bool{false, true} {
			name := transport + "/socket"
			if alias {
				name = transport + "/monitor"
			}
			t.Run(name, func(t *testing.T) {
				check := func(err error) {
					t.Helper()
					if err != nil {
						t.Fatal(err)
					}
				}
				ctx := newContext(t)
				defer ctx.Close()
				server, err := ctx.RouterSocket()
				check(err)
				defer server.Close()
				client, err := ctx.DealerSocket()
				check(err)
				defer client.Close()
				monitor, err := zlink.OpenSocketMonitor(client, zlink.MonitorEventConnectionReady, zlink.MonitorEventDisconnected)
				check(err)
				defer monitor.Close()
				poller, err := zlink.NewPoller()
				check(err)
				defer poller.Close()
				add := func() error { return poller.AddSocket(monitor, zlink.PollIn, 42) }
				modify := func() error { return poller.ModifySocket(monitor, zlink.PollIn) }
				remove := func() error { return poller.RemoveSocket(monitor) }
				if alias {
					add = func() error { return poller.AddMonitor(monitor, zlink.PollIn, 42) }
					modify = func() error { return poller.ModifyMonitor(monitor, zlink.PollIn) }
					remove = func() error { return poller.RemoveMonitor(monitor) }
				}
				endpoint := inprocEndpoint("monitor-poller")
				if transport == "tcp" {
					endpoint = tcpEndpoint(t)
				}
				check(server.Bind(endpoint))
				check(client.Connect(endpoint))
				check(add())
				check(modify())
				if poller.Size() != 1 {
					t.Fatal("expected one registered monitor")
				}
				events := make([]zlink.PollEvent, 1)
				drain := func(expected zlink.MonitorEventType) {
					t.Helper()
					n, err := poller.Wait(events, 2*time.Second)
					check(err)
					if n != 1 || events[0].Slot != 42 || events[0].SourceKind != zlink.PollSourceSocket || events[0].Revents != zlink.PollIn {
						t.Fatalf("monitor readiness: n=%d events=%+v", n, events)
					}
					found := false
					for {
						event, err := monitor.Recv(zlink.RecvFlagsDontWait)
						var recvErr *zlink.RecvError
						if errors.As(err, &recvErr) && recvErr.Result == zlink.RecvNoData {
							break
						}
						check(err)
						if event == nil {
							break
						}
						if event.Event == expected {
							found = true
						}
					}
					if !found {
						t.Fatalf("missing monitor event %v", expected)
					}
					n, err = poller.Wait(events, 0)
					check(err)
					if n != 0 {
						t.Fatal("drained monitor remains ready")
					}
				}
				drain(zlink.MonitorEventTypeConnectionReady)
				check(server.Close())
				drain(zlink.MonitorEventTypeDisconnected)
				check(remove())
				if poller.Size() != 0 {
					t.Fatal("monitor still registered")
				}
				replacement, err := ctx.RouterSocket()
				check(err)
				defer replacement.Close()
				check(replacement.Bind(endpoint))
				check(add())
				n, err := poller.Wait(events, 2*time.Second)
				check(err)
				if n != 1 {
					t.Fatal("replacement connection did not become ready")
				}
				check(remove())
				n, err = poller.Wait(events, 0)
				check(err)
				if n != 0 {
					t.Fatal("removed monitor was delivered")
				}
				event, err := monitor.Recv(zlink.RecvFlagsDontWait)
				check(err)
				if event == nil || !event.IsConnectionReady() {
					t.Fatalf("removed monitor should retain READY: %+v", event)
				}
			})
		}
	}
}

func TestMonitorPollerRejectsInvalidFlags(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	socket, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer socket.Close()
	monitor, err := zlink.OpenSocketMonitor(socket)
	if err != nil {
		t.Fatal(err)
	}
	defer monitor.Close()
	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	invalid := func(err error) {
		t.Helper()
		var typed *zlink.ConfigError
		if !errors.As(err, &typed) || typed.Result != zlink.ConfigInvalidArgument {
			t.Fatalf("expected ConfigInvalidArgument, got %v", err)
		}
	}
	for _, flags := range []zlink.PollEventFlag{zlink.PollOut, zlink.PollCompletion, zlink.PollIn | zlink.PollOut} {
		invalid(poller.AddMonitor(monitor, flags, 1))
		invalid(poller.AddSocket(monitor, flags, 1))
		if poller.Size() != 0 {
			t.Fatal("invalid add registered monitor")
		}
		if err := poller.AddMonitor(monitor, zlink.PollIn, 1); err != nil {
			t.Fatal(err)
		}
		invalid(poller.ModifyMonitor(monitor, flags))
		invalid(poller.ModifySocket(monitor, flags))
		if poller.Size() != 1 {
			t.Fatal("invalid modify lost registration")
		}
		if err := poller.RemoveMonitor(monitor); err != nil {
			t.Fatal(err)
		}
	}
}
