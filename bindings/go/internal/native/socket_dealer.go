// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"context"
	"time"
	"unsafe"
)

type DealerSocket struct {
	*connectionSocket
}

func newDealerSocket(ctx *Context) (*DealerSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_DEALER)
	if err != nil {
		return nil, err
	}
	return &DealerSocket{connectionSocket: &connectionSocket{socketCore: core}}, nil
}

func (s *DealerSocket) SetRoutingID(id RoutingID) error {
	raw := routingIDToC(id)
	nativeResult1, nativeErr1 := C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size))
	return configErrorFromCall(nativeResult1, nativeErr1)
}

func (s *DealerSocket) SetProbe(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	nativeResult2, nativeErr2 := C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_PROBE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult2, nativeErr2)
}

func (s *DealerSocket) Probe() (bool, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	rc, errno := C.zlink_get_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_PROBE, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(rc, errno); err != nil {
		return false, err
	}
	return raw != 0, nil
}

func (s *DealerSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}

func (s *DealerSocket) SetWeight(value int) error {
	raw := C.int(value)
	nativeResult3, nativeErr3 := C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_WEIGHT, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult3, nativeErr3)
}

func (s *DealerSocket) Weight() (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult4, nativeErr4 := C.zlink_get_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_WEIGHT, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult4, nativeErr4); err != nil {
		return 0, err
	}
	return int(raw), nil
}

func (s *DealerSocket) SetRequestTimeout(value time.Duration) error {
	ms, err := durationToMillis(value)
	if err != nil {
		return err
	}
	raw := C.int(ms)
	nativeResult5, nativeErr5 := C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult5, nativeErr5)
}

func (s *DealerSocket) RequestTimeout() (time.Duration, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	rc, errno := C.zlink_get_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(rc, errno); err != nil {
		return 0, err
	}
	return time.Duration(raw) * time.Millisecond, nil
}

func (s *DealerSocket) Send() SendOp {
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) (SendSubmission, error) {
		return submitManagedSend(ctx, s.socketCore, nil, parts)
	})
}

func (s *DealerSocket) Request() RequestOp {
	return newRequestBuilder(func(ctx context.Context, parts []requestBuilderPart, timeout time.Duration) (RequestSubmission, error) {
		return submitCompletionRequest(ctx, s.socketCore, nil, timeout, parts)
	})
}
