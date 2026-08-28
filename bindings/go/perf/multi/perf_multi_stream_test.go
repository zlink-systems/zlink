package main

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestMultiStreamRouteDispatchIsolatesBlockedRoute(t *testing.T) {
	routeA := zlink.NewRoutingIDString("route-a")
	routeB := zlink.NewRoutingIDString("route-b")
	releaseA := make(chan struct{})
	routeBSubmitted := make(chan struct{}, 1)

	var mu sync.Mutex
	counts := make(map[zlink.RoutingID]int)
	dispatch := newMultiStreamRouteDispatchWithSubmit(
		func(source zlink.RoutingID, _ *zlink.Message) (bool, error) {
			if source.Equal(routeA) {
				<-releaseA
			}
			mu.Lock()
			counts[source]++
			mu.Unlock()
			if source.Equal(routeB) {
				select {
				case routeBSubmitted <- struct{}{}:
				default:
				}
			}
			return true, nil
		},
	)

	const queuedOnA = 128
	for i := 0; i < queuedOnA; i++ {
		packet, err := zlink.NewMessage([]byte("a"))
		if err != nil {
			t.Fatalf("NewMessage(route A): %v", err)
		}
		dispatch.enqueue(routeA, packet)
	}
	packetB, err := zlink.NewMessage([]byte("b"))
	if err != nil {
		t.Fatalf("NewMessage(route B): %v", err)
	}
	dispatch.enqueue(routeB, packetB)

	select {
	case <-routeBSubmitted:
	case <-time.After(time.Second):
		t.Fatal("route B did not progress while route A was blocked")
	}

	close(releaseA)
	if err := dispatch.stop(); err != nil {
		t.Fatalf("stop(): %v", err)
	}
	mu.Lock()
	defer mu.Unlock()
	if counts[routeA] != queuedOnA {
		t.Fatalf("route A submitted %d packets, want %d", counts[routeA], queuedOnA)
	}
	if counts[routeB] != 1 {
		t.Fatalf("route B submitted %d packets, want 1", counts[routeB])
	}
}

func TestMultiStreamRouteDispatchDropsOnlyStaleRoute(t *testing.T) {
	routeA := zlink.NewRoutingIDString("stale-route")
	routeB := zlink.NewRoutingIDString("live-route")
	releaseA := make(chan struct{})
	routeBSubmitted := make(chan struct{}, 1)
	var routeACalls atomic.Int64
	var routeBCalls atomic.Int64

	dispatch := newMultiStreamRouteDispatchWithSubmit(
		func(source zlink.RoutingID, _ *zlink.Message) (bool, error) {
			if source.Equal(routeA) {
				<-releaseA
				routeACalls.Add(1)
				return false, &zlink.SubmitError{Result: zlink.SubmitNotFound}
			}
			routeBCalls.Add(1)
			routeBSubmitted <- struct{}{}
			return true, nil
		},
	)

	const queuedOnA = 64
	for i := 0; i < queuedOnA; i++ {
		packet, err := zlink.NewMessage([]byte("stale"))
		if err != nil {
			t.Fatalf("NewMessage(stale route): %v", err)
		}
		dispatch.enqueue(routeA, packet)
	}
	packetB, err := zlink.NewMessage([]byte("live"))
	if err != nil {
		t.Fatalf("NewMessage(live route): %v", err)
	}
	dispatch.enqueue(routeB, packetB)

	select {
	case <-routeBSubmitted:
	case <-time.After(time.Second):
		t.Fatal("live route did not progress while stale route was blocked")
	}
	close(releaseA)
	if err := dispatch.stop(); err != nil {
		t.Fatalf("stop(): %v", err)
	}
	if got := routeACalls.Load(); got != 1 {
		t.Fatalf("stale route submitted %d packets, want only the head packet", got)
	}
	if got := routeBCalls.Load(); got != 1 {
		t.Fatalf("live route submitted %d packets, want 1", got)
	}
	select {
	case err := <-dispatch.errors:
		t.Fatalf("stale route surfaced as fatal error: %v", err)
	default:
	}
}
