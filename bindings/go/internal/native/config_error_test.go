// SPDX-License-Identifier: MPL-2.0

package native

import "testing"

func TestCoreResultDoesNotSynthesizeMissingErrno(t *testing.T) {
	tests := []struct {
		name string
		err  error
	}{
		{name: "submit", err: submitErrorFromCall(SubmitBackpressured, nil)},
		{name: "recv", err: recvErrorFromCall(RecvNoData, nil)},
		{name: "handler", err: handlerErrorFromCall(HandlerBusy, nil)},
		{name: "close", err: closeErrorFromCall(CloseBusy, nil)},
		{name: "bind", err: bindErrorFromCall(BindAddrInUse, nil)},
		{name: "connect", err: connectErrorFromCall(ConnectBusy, nil)},
		{name: "config", err: configErrorFromCall(ConfigInvalidState, nil)},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := test.err.(interface{ InternalErrno() int }).InternalErrno(); got != 0 {
				t.Fatalf("synthetic errno = %d, want 0", got)
			}
		})
	}
}

func TestUnclassifiedConstructorErrnoIsInternalError(t *testing.T) {
	for _, errno := range []int{0, 0x7fffffff} {
		err := configErrorFromErrno(errno)
		config, ok := err.(*ConfigError)
		if !ok {
			t.Fatalf("errno %d: expected ConfigError, got %T", errno, err)
		}
		if config.Result != ConfigInternalError || config.InternalErrno() != errno {
			t.Fatalf("errno %d: got result %v, native errno %d", errno,
				config.Result, config.InternalErrno())
		}
	}
}
