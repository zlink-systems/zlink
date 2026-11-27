// SPDX-License-Identifier: MPL-2.0

package zlink_test

// Projection of the Core connection-attempt snapshot and POLLCONNECT
// (bindings spec §Socket Readiness Snapshots). Scenario mirrors
// bindings/node/tests/connection_snapshot.test.ts.

import (
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

type connectionSnapshotter interface {
	ConnectionAttemptsSnapshot() ([]zlink.ConnectionAttempt, error)
}

func TestConnectionAttemptsSnapshotOnEveryRawFamilyButStream(t *testing.T) {
	ctx, err := zlink.NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	defer ctx.Close()
	pair, _ := ctx.PairSocket()
	pub, _ := ctx.PubSocket()
	sub, _ := ctx.SubSocket()
	xpub, _ := ctx.XPubSocket()
	xsub, _ := ctx.XSubSocket()
	dealer, _ := ctx.DealerSocket()
	router, _ := ctx.RouterSocket()
	type closable interface {
		connectionSnapshotter
		Close() error
	}
	for i, socket := range []closable{pair, pub, sub, xpub, xsub, dealer, router} {
		rows, err := socket.ConnectionAttemptsSnapshot()
		if err != nil || len(rows) != 0 {
			t.Fatalf("socket %d snapshot = (%v, %v), want empty", i, rows, err)
		}
		if err := socket.Close(); err != nil {
			t.Fatalf("Close() error = %v", err)
		}
		_, err = socket.ConnectionAttemptsSnapshot()
		var cfg *zlink.ConfigError
		if !errors.As(err, &cfg) || cfg.Result != zlink.ConfigInvalidHandle {
			t.Fatalf("socket %d closed snapshot error = %v, want ConfigInvalidHandle", i, err)
		}
	}
	stream, err := ctx.StreamSocket()
	if err != nil {
		t.Fatalf("StreamSocket() error = %v", err)
	}
	defer stream.Close()
	var anyStream any = stream
	if _, ok := anyStream.(connectionSnapshotter); ok {
		t.Fatal("StreamSocket must not expose ConnectionAttemptsSnapshot")
	}
}

func TestPollConnectValueAndConnectedSnapshot(t *testing.T) {
	if zlink.PollConnect != 128 {
		t.Fatalf("PollConnect = %d, want 128", zlink.PollConnect)
	}
	ctx, err := zlink.NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	defer ctx.Close()
	router, err := ctx.RouterSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer dealer.Close()
	poller, err := zlink.NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	if err := router.Bind("tcp://127.0.0.1:*"); err != nil {
		t.Fatal(err)
	}
	endpoint, err := router.CommonOptions().LastEndpoint()
	if err != nil {
		t.Fatal(err)
	}
	if err := poller.AddSocket(dealer, zlink.PollConnect, 10); err != nil {
		t.Fatalf("AddSocket(PollConnect) error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect(%q) error = %v", endpoint, err)
	}
	events := make([]zlink.PollEvent, 1)
	deadline := time.Now().Add(5 * time.Second)
	var row zlink.ConnectionAttempt
	for found := false; !found; {
		if time.Now().After(deadline) {
			t.Fatal("connected attempt was not observed")
		}
		n, err := poller.Wait(events, 50*time.Millisecond)
		if err != nil {
			t.Fatal(err)
		}
		if n == 0 || events[0].Revents&zlink.PollConnect == 0 {
			continue
		}
		rows, err := dealer.ConnectionAttemptsSnapshot()
		if err != nil {
			t.Fatal(err)
		}
		for _, r := range rows {
			if r.State == zlink.ConnectionConnected {
				row, found = r, true
			}
		}
	}
	if row.Endpoint != endpoint || row.IntentID == 0 || row.Revision == 0 || row.LastFailureID != 0 ||
		row.LastFailure.Kind != zlink.ConnectionFailureNone || row.LastFailure.Stage != zlink.ConnectionStageNone {
		t.Fatalf("connected row = %+v", row)
	}
	// A snapshot that saw every change releases readiness.
	if n, err := poller.Wait(events, 0); err != nil || n != 0 {
		t.Fatalf("Wait after snapshot = (%d, %v), want idle", n, err)
	}
	if err := dealer.Disconnect(endpoint); err != nil {
		t.Fatal(err)
	}
	rows, err := dealer.ConnectionAttemptsSnapshot()
	if err != nil || len(rows) != 0 {
		t.Fatalf("snapshot after disconnect = (%v, %v), want empty", rows, err)
	}
}

func TestConnectionAttemptsSnapshotGrowsRowAndEndpointArenas(t *testing.T) {
	ctx, err := zlink.NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	defer ctx.Close()
	dealer, err := ctx.DealerSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer dealer.Close()
	want := map[string]bool{}
	for i := 0; i < 40; i++ {
		endpoint := fmt.Sprintf("inproc://connection-%d-%s", i, strings.Repeat("x", 80))
		want[endpoint] = true
		if err := dealer.Connect(endpoint); err != nil {
			t.Fatal(err)
		}
	}
	rows, err := dealer.ConnectionAttemptsSnapshot()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != len(want) {
		t.Fatalf("snapshot rows = %d, want %d", len(rows), len(want))
	}
	for _, r := range rows {
		if !want[r.Endpoint] || r.IntentID == 0 || r.Revision == 0 {
			t.Fatalf("unexpected row %+v", r)
		}
		delete(want, r.Endpoint)
	}
}
