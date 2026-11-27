// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"time"
	"unsafe"
)

type RouterSocket struct {
	*routedSocket
}

func newRouterSocket(ctx *Context) (*RouterSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_ROUTER)
	if err != nil {
		return nil, err
	}
	return &RouterSocket{
		routedSocket: &routedSocket{
			connectionSocket: &connectionSocket{socketCore: core},
			replyOwner:       &replyTokenOwner{marker: 1},
		},
	}, nil
}

func (s *RouterSocket) SetMandatory(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	nativeResult6, nativeErr6 := C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_MANDATORY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult6, nativeErr6)
}

func (s *RouterSocket) Mandatory() (bool, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	rc, errno := C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_MANDATORY, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(rc, errno); err != nil {
		return false, err
	}
	return raw != 0, nil
}

func (s *RouterSocket) SetProbe(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	nativeResult7, nativeErr7 := C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_PROBE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult7, nativeErr7)
}

func (s *RouterSocket) Probe() (bool, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	rc, errno := C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_PROBE, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(rc, errno); err != nil {
		return false, err
	}
	return raw != 0, nil
}

func (s *RouterSocket) SetRoutingID(id RoutingID) error {
	raw := routingIDToC(id)
	nativeResult8, nativeErr8 := C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size))
	return configErrorFromCall(nativeResult8, nativeErr8)
}

func (s *RouterSocket) SetWeight(value int) error {
	raw := C.int(value)
	nativeResult9, nativeErr9 := C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_WEIGHT, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult9, nativeErr9)
}

func (s *RouterSocket) Weight() (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult10, nativeErr10 := C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_WEIGHT, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult10, nativeErr10); err != nil {
		return 0, err
	}
	return int(raw), nil
}

func (s *RouterSocket) SetRequestTimeout(value time.Duration) error {
	ms, err := durationToMillis(value)
	if err != nil {
		return err
	}
	raw := C.int(ms)
	nativeResult11, nativeErr11 := C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult11, nativeErr11)
}

func (s *RouterSocket) RequestTimeout() (time.Duration, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult12, nativeErr12 := C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult12, nativeErr12); err != nil {
		return 0, err
	}
	return time.Duration(raw) * time.Millisecond, nil
}

func (s *RouterSocket) SetHandover(value bool) error {
	var raw C.int
	if value {
		raw = C.int(C.ZLINK_RID_DUPLICATE_HANDOVER)
	}
	nativeResult13, nativeErr13 := C.zlink_set_option(s.raw(), C.ZLINK_OPT_RID_DUPLICATE_POLICY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult13, nativeErr13)
}

func (s *RouterSocket) Handover() (bool, error) {
	value, err := s.CommonOptions().RidDuplicatePolicy()
	return value == RidDuplicateHandover, err
}

func (s *RouterSocket) SetConnectRoutingID(id RoutingID) error {
	raw := routingIDToC(id)
	nativeResult14, nativeErr14 := C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, routingIDPointer(&raw), C.size_t(raw.size))
	return configErrorFromCall(nativeResult14, nativeErr14)
}

func (s *RouterSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}
