// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"context"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestIndependentConcurrentSubmitsKeepMultipartRecordsIntact(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	receiver, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer receiver.Close()
	sender, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()

	endpoint := inprocEndpoint("concurrent-multipart")
	if err := receiver.Bind(endpoint); err != nil {
		t.Fatal(err)
	}
	if err := receiver.CommonOptions().SetReceiveTimeout(2 * time.Second); err != nil {
		t.Fatal(err)
	}
	if err := sender.Connect(endpoint); err != nil {
		t.Fatal(err)
	}

	start := make(chan struct{})
	done := make(chan error, 2)
	for _, id := range []string{"a", "b"} {
		op := sender.Send().Bytes([]byte(id)).Bytes([]byte(id + "-body"))
		go func() {
			<-start
			done <- submitAndWait(context.Background(), op)
		}()
	}
	close(start)
	for range 2 {
		if err := <-done; err != nil {
			t.Error(err)
		}
	}
	if t.Failed() {
		return
	}

	want := map[string]string{"a": "a-body", "b": "b-body"}
	var received zlink.Received
	defer received.Close()
	for range 2 {
		ok, err := receiver.Recv(&received, zlink.RecvFlagsNone)
		if err != nil || !ok {
			t.Fatalf("Recv = (%t, %v)", ok, err)
		}
		parts := received.Parts()
		if len(parts) != 2 {
			t.Fatalf("record has %d parts, want 2", len(parts))
		}
		id, body := parts[0].Text(), parts[1].Text()
		if expected, found := want[id]; !found || body != expected {
			t.Fatalf("unexpected record (%q, %q)", id, body)
		}
		delete(want, id)
	}
}
