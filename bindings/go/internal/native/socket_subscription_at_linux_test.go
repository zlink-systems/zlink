//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"strings"
	"syscall"
	"testing"

	"zlink.systems/zlink/internal/native/subscriptiontest"
)

func TestSubscriptionAtCoreCalls(t *testing.T) {
	longFilter := strings.Repeat("x", 4096)
	for _, test := range []struct {
		name       string
		filter     string
		index      int
		injected   ConfigResult
		first, end ConfigResult
		calls      int
	}{
		{name: "missing", index: 1, first: ConfigNotFound, calls: 1},
		{name: "empty", first: ConfigOK, calls: 1},
		{name: "long", filter: longFilter, first: ConfigBufferTooSmall, end: ConfigOK, calls: 2},
		{name: "removed before retry", filter: longFilter, injected: ConfigNotFound, first: ConfigBufferTooSmall, end: ConfigNotFound, calls: 2},
		{name: "invalid argument", injected: ConfigInvalidArgument, first: ConfigInvalidArgument, calls: 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			ctx, err := NewContext()
			if err != nil {
				t.Fatal(err)
			}
			defer ctx.Close()
			socket, err := ctx.SubSocket()
			if err != nil {
				t.Fatal(err)
			}
			defer socket.Close()
			if err := socket.SetSubscription(test.filter); err != nil {
				t.Fatal(err)
			}

			subscriptiontest.Start(socket.raw(), int(test.injected))
			defer subscriptiontest.Stop()
			entry, found, err := socket.SubscriptionAt(test.index)
			if test.injected == ConfigInvalidArgument {
				var configErr *ConfigError
				if !errors.As(err, &configErr) || configErr.Result != ConfigInvalidArgument || !errors.Is(err, syscall.EINVAL) {
					t.Errorf("expected ConfigInvalidArgument/EINVAL, got %v", err)
				}
			} else if err != nil {
				t.Errorf("SubscriptionAt(%d) returned %v", test.index, err)
			}
			if got := subscriptiontest.Calls(); got != test.calls {
				t.Errorf("Core calls = %d, want %d", got, test.calls)
			}
			if got := subscriptiontest.Capacity(0); got <= 0 {
				t.Errorf("first Core buffer capacity = %d, want a positive capacity", got)
			}
			if got := ConfigResult(subscriptiontest.Result(0)); got != test.first {
				t.Errorf("first Core result = %v, want %v", got, test.first)
			}
			if test.calls == 2 {
				if got := subscriptiontest.Capacity(1); got != len(test.filter) {
					t.Errorf("retry capacity = %d, want %d", got, len(test.filter))
				}
				if got := ConfigResult(subscriptiontest.Result(1)); got != test.end {
					t.Errorf("retry Core result = %v, want %v", got, test.end)
				}
			}
			wantFound := test.index == 0 && test.injected == ConfigOK
			wantEntry := Subscription{}
			if wantFound {
				wantEntry.Filter = test.filter
			}
			if found != wantFound || entry != wantEntry {
				t.Errorf("SubscriptionAt(%d) = (%+v, %v), want (%+v, %v)", test.index, entry, found, wantEntry, wantFound)
			}
		})
	}
}
