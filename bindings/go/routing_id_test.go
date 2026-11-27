// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestTryNewRoutingIDReportsInvalidSizeAtCreation(t *testing.T) {
	for _, size := range []int{0, 256} {
		rid, err := zlink.TryNewRoutingID(make([]byte, size))
		var cfg *zlink.ConfigError
		if !errors.As(err, &cfg) || cfg.Result != zlink.ConfigInvalidArgument {
			t.Fatalf("TryNewRoutingID(%d bytes) error = %v, want ConfigInvalidArgument", size, err)
		}
		if rid.Size() != 0 {
			t.Fatalf("TryNewRoutingID(%d bytes) size = %d, want 0", size, rid.Size())
		}
		if zlink.NewRoutingID(make([]byte, size)).Size() != 0 {
			t.Fatalf("NewRoutingID(%d bytes) must keep returning the empty value", size)
		}
	}
	for _, size := range []int{1, 255} {
		rid, err := zlink.TryNewRoutingID(bytes.Repeat([]byte{7}, size))
		if err != nil || rid.Size() != size {
			t.Fatalf("TryNewRoutingID(%d bytes) = (%d, %v)", size, rid.Size(), err)
		}
	}
}

func TestTryNewRoutingIDFromHexReportsInvalidInput(t *testing.T) {
	for _, text := range []string{"", "abc", "zz", strings.Repeat("ab", 256)} {
		if _, err := zlink.TryNewRoutingIDFromHex(text); err == nil {
			t.Fatalf("TryNewRoutingIDFromHex(%.8q) error = nil", text)
		}
	}
	rid, err := zlink.TryNewRoutingIDFromHex(strings.Repeat("ab", 255))
	if err != nil || rid.Size() != 255 {
		t.Fatalf("TryNewRoutingIDFromHex(510 chars) = (%d, %v)", rid.Size(), err)
	}
}

func TestRoutingIDTypedAccessors(t *testing.T) {
	if v, ok := zlink.NewRoutingIDUint32(0xA1B2C3D4).Uint32(); !ok || v != 0xA1B2C3D4 {
		t.Fatalf("Uint32() = (%x, %v)", v, ok)
	}
	if _, ok := zlink.NewRoutingIDString("abc").Uint32(); ok {
		t.Fatal("3-byte routing id must not convert to uint32")
	}
	uuid := [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	if v, ok := zlink.NewRoutingIDUUIDBytes(uuid).UUIDBytes(); !ok || v != uuid {
		t.Fatalf("UUIDBytes() = (%v, %v)", v, ok)
	}
	if _, ok := zlink.NewRoutingIDUint32(1).UUIDBytes(); ok {
		t.Fatal("4-byte routing id must not convert to UUID bytes")
	}
}
