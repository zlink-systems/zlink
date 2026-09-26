// SPDX-License-Identifier: MPL-2.0

package zlink_test

// Projection of the Core ROUTER selected-route observation (Core ROUTER
// §10.1): RouterSocket.RoutesSnapshot(), PollRoute and the route generation
// carried on a received ROUTER record. Scenario mirrors
// bindings/cpp/tests/contract/test_cpp_contract_router_selected_route.cpp.

import (
	"context"
	"fmt"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

const routeWaitLimit = 5 * time.Second

func newRouteRouter(t testing.TB, ctx *zlink.Context, name string) *zlink.RouterSocket {
	t.Helper()
	socket, err := ctx.RouterSocket()
	if err != nil {
		t.Fatalf("RouterSocket() error = %v", err)
	}
	if err := socket.SetRoutingID(zlink.NewRoutingIDString(name)); err != nil {
		t.Fatalf("SetRoutingID() error = %v", err)
	}
	if err := socket.CommonOptions().SetLinger(0); err != nil {
		t.Fatalf("SetLinger() error = %v", err)
	}
	if err := socket.CommonOptions().SetReconnectInterval(0); err != nil {
		t.Fatalf("SetReconnectInterval() error = %v", err)
	}
	if err := socket.CommonOptions().SetReceiveTimeout(routeWaitLimit); err != nil {
		t.Fatalf("SetReceiveTimeout() error = %v", err)
	}
	if err := socket.SetHandover(true); err != nil {
		t.Fatalf("SetHandover() error = %v", err)
	}
	return socket
}

func connectAs(t testing.TB, source *zlink.RouterSocket, peer string, endpoint string) {
	t.Helper()
	if err := source.SetConnectRoutingID(zlink.NewRoutingIDString(peer)); err != nil {
		t.Fatalf("SetConnectRoutingID() error = %v", err)
	}
	if err := source.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
}

func findGeneration(t testing.TB, routes []zlink.RouterRoute, peer string) uint64 {
	t.Helper()
	expected := zlink.NewRoutingIDString(peer)
	var found uint64
	matches := 0
	for _, route := range routes {
		if route.RouteGeneration == 0 {
			t.Fatalf("route generation for %q is 0", route.RoutingID.String())
		}
		if route.RoutingID.Equal(expected) {
			matches++
			found = route.RouteGeneration
		}
	}
	if matches > 1 {
		t.Fatalf("more than one selected route for %q", peer)
	}
	return found
}

// waitRoute polls PollRoute on poller until peer's selected route has a
// generation other than old; the socket must already be registered on
// poller for PollRoute. Core applies route changes while the socket is
// polled, so callers with no poller of their own get one here.
func waitRoute(t testing.TB, socket *zlink.RouterSocket, poller *zlink.Poller, peer string, old uint64) uint64 {
	t.Helper()
	owned := poller
	if owned == nil {
		created, err := zlink.NewPoller()
		if err != nil {
			t.Fatalf("NewPoller() error = %v", err)
		}
		defer created.Close()
		if err := created.AddSocket(socket, zlink.PollRoute, 1); err != nil {
			t.Fatalf("AddSocket() error = %v", err)
		}
		owned = created
	}
	deadline := time.Now().Add(routeWaitLimit)
	events := make([]zlink.PollEvent, 1)
	for {
		routes, err := socket.RoutesSnapshot()
		if err != nil {
			t.Fatalf("RoutesSnapshot() error = %v", err)
		}
		generation := findGeneration(t, routes, peer)
		if generation != 0 && generation != old {
			return generation
		}
		if time.Now().After(deadline) {
			t.Fatalf("selected route of %q did not change from %d", peer, old)
		}
		if _, err := owned.Wait(events, 10*time.Millisecond); err != nil {
			t.Fatalf("Wait() error = %v", err)
		}
	}
}

func routeReadyNow(t testing.TB, poller *zlink.Poller) bool {
	t.Helper()
	events := make([]zlink.PollEvent, 1)
	n, err := poller.Wait(events, 0)
	if err != nil {
		t.Fatalf("Wait() error = %v", err)
	}
	return n == 1 && events[0].Revents&zlink.PollRoute != 0
}

func sendRouteText(t testing.TB, socket *zlink.RouterSocket, peer string, text string) {
	t.Helper()
	msg, err := zlink.NewMessage([]byte(text))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	if err := submitAndWait(context.Background(), socket.SendTo(zlink.NewRoutingIDString(peer)).Message(msg)); err != nil {
		t.Fatalf("SendTo() error = %v", err)
	}
}

func recvRouteText(t testing.TB, socket *zlink.RouterSocket, expected string) *zlink.Received {
	t.Helper()
	received := &zlink.Received{}
	ok, err := socket.Recv(received, zlink.RecvFlagsNone)
	if err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	if !ok {
		t.Fatalf("Recv() returned no data")
	}
	if !received.IsSinglePart() {
		t.Fatalf("IsSinglePart() = false")
	}
	part, err := received.FirstPart()
	if err != nil {
		t.Fatalf("FirstPart() error = %v", err)
	}
	if got := string(part.Data()); got != expected {
		t.Fatalf("payload = %q, want %q", got, expected)
	}
	return received
}

func TestRouterSelectedRouteSnapshotPollRouteAndRecordGeneration(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	server := newRouteRouter(t, ctx, "route-server")
	defer server.Close()
	client := newRouteRouter(t, ctx, "route-client")
	defer client.Close()

	endpoint := tcpEndpoint(t)
	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	initial, err := client.RoutesSnapshot()
	if err != nil {
		t.Fatalf("RoutesSnapshot() error = %v", err)
	}
	if len(initial) != 0 {
		t.Fatalf("RoutesSnapshot() before connect = %v, want empty", initial)
	}

	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	defer poller.Close()
	if err := poller.AddSocket(client, zlink.PollRoute, 7); err != nil {
		t.Fatalf("AddSocket() error = %v", err)
	}
	connectAs(t, client, "route-server", endpoint)

	events := make([]zlink.PollEvent, 1)
	n, err := poller.Wait(events, routeWaitLimit)
	if err != nil {
		t.Fatalf("Wait() error = %v", err)
	}
	if n != 1 || events[0].Slot != 7 || events[0].Revents&zlink.PollRoute == 0 {
		t.Fatalf("Wait() = (%d, %+v), want PollRoute on slot 7", n, events)
	}

	routes, err := client.RoutesSnapshot()
	if err != nil {
		t.Fatalf("RoutesSnapshot() error = %v", err)
	}
	if len(routes) != 1 {
		t.Fatalf("RoutesSnapshot() = %v, want one row", routes)
	}
	selected := findGeneration(t, routes, "route-server")
	if selected == 0 {
		t.Fatal("selected route generation is 0")
	}
	// A successful snapshot that saw every change clears the level readiness.
	if routeReadyNow(t, poller) {
		t.Fatal("PollRoute still ready after a snapshot that saw every change")
	}

	waitRoute(t, server, nil, "route-client", 0)
	sendRouteText(t, server, "route-client", "selected")
	received := recvRouteText(t, client, "selected")
	defer received.Close()
	if received.RouteGeneration() != selected {
		t.Fatalf("RouteGeneration() = %d, want %d", received.RouteGeneration(), selected)
	}

	// A DEALER record carries no ROUTER route generation.
	dealer, err := ctx.DealerSocket()
	if err != nil {
		t.Fatalf("DealerSocket() error = %v", err)
	}
	defer dealer.Close()
	if err := dealer.SetRoutingID(zlink.NewRoutingIDString("route-dealer")); err != nil {
		t.Fatalf("SetRoutingID() error = %v", err)
	}
	if err := dealer.CommonOptions().SetLinger(0); err != nil {
		t.Fatalf("SetLinger() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	waitRoute(t, server, nil, "route-dealer", 0)
	sendRouteText(t, server, "route-dealer", "dealer")
	var dealerReceived zlink.Received
	if err := dealer.CommonOptions().SetReceiveTimeout(routeWaitLimit); err != nil {
		t.Fatalf("SetReceiveTimeout() error = %v", err)
	}
	ok, err := dealer.Recv(&dealerReceived, zlink.RecvFlagsNone)
	if err != nil {
		t.Fatalf("Recv() error = %v", err)
	}
	if !ok {
		t.Fatal("Recv() returned no data")
	}
	defer dealerReceived.Close()
	if dealerReceived.RouteGeneration() != 0 {
		t.Fatalf("dealer RouteGeneration() = %d, want 0", dealerReceived.RouteGeneration())
	}
}

func TestRouterSelectedRouteReplacedRouteChangesGeneration(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	oldServer := newRouteRouter(t, ctx, "route-server")
	defer oldServer.Close()
	newServer := newRouteRouter(t, ctx, "route-server")
	defer newServer.Close()
	client := newRouteRouter(t, ctx, "route-client")
	defer client.Close()

	oldEndpoint := tcpEndpoint(t)
	if err := oldServer.Bind(oldEndpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	newEndpoint := tcpEndpoint(t)
	if err := newServer.Bind(newEndpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}

	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatalf("NewPoller() error = %v", err)
	}
	defer poller.Close()
	if err := poller.AddSocket(client, zlink.PollRoute, 9); err != nil {
		t.Fatalf("AddSocket() error = %v", err)
	}
	connectAs(t, client, "route-server", oldEndpoint)
	first := waitRoute(t, client, poller, "route-server", 0)

	// Handover: the new pipe for the same RID takes over the selected route.
	connectAs(t, client, "route-server", newEndpoint)
	second := waitRoute(t, client, poller, "route-server", first)
	if second == first {
		t.Fatal("replaced route generation did not change")
	}

	waitRoute(t, newServer, nil, "route-client", 0)
	sendRouteText(t, newServer, "route-client", "new")
	received := recvRouteText(t, client, "new")
	if received.RouteGeneration() != second {
		t.Fatalf("RouteGeneration() = %d, want %d", received.RouteGeneration(), second)
	}
	received.Close()

	// The selected pipe ends: the retained standby is promoted with a new generation.
	if err := newServer.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	promoted := waitRoute(t, client, poller, "route-server", second)
	if promoted == second {
		t.Fatal("promoted route generation did not change")
	}
}

func TestRouterSelectedRouteSnapshotGrowsPastInitialCapacity(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	client := newRouteRouter(t, ctx, "route-client")
	defer client.Close()

	const serverCount = 20
	names := make([]string, serverCount)
	for index := 0; index < serverCount; index++ {
		name := fmt.Sprintf("route-server-%d", index)
		names[index] = name
		server := newRouteRouter(t, ctx, name)
		defer server.Close()
		endpoint := tcpEndpoint(t)
		if err := server.Bind(endpoint); err != nil {
			t.Fatalf("Bind() error = %v", err)
		}
		connectAs(t, client, name, endpoint)
	}
	for _, name := range names {
		waitRoute(t, client, nil, name, 0)
	}
	routes, err := client.RoutesSnapshot()
	if err != nil {
		t.Fatalf("RoutesSnapshot() error = %v", err)
	}
	if len(routes) != serverCount {
		t.Fatalf("RoutesSnapshot() returned %d routes, want %d", len(routes), serverCount)
	}
}
