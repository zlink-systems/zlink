//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"testing"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestContextCloseShutsDownBeforeTerm(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	completiontest.Start(nil)
	defer completiontest.Stop()
	completiontest.WatchContext(ctx.raw())
	if err := ctx.Close(); err != nil {
		t.Fatal(err)
	}
	if got := completiontest.Trace(); got != "ST" {
		t.Fatalf("context native close calls = %q, want shutdown then term", got)
	}
}

func TestContextCloseLeavesStandalonePollerOpen(t *testing.T) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatal(err)
	}
	poller, err := NewPoller()
	if err != nil {
		t.Fatal(err)
	}
	defer poller.Close()
	if err := ctx.Close(); err != nil {
		t.Fatal(err)
	}
	if size, err := poller.Size(); err != nil || size != 0 {
		t.Fatalf("poller after context close: size=%d err=%v", size, err)
	}
}
