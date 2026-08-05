// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"

extern void goZlinkStreamPacketTrampoline(void *stream_, const zlink_routing_id_t *source_rid_, zlink_msg_t *header_, zlink_msg_t *body_, uintptr_t userdata_);

static inline int zlink_stream_packet_handler_go_local(void *s, uintptr_t userdata) {
    return zlink_stream_packet_handler(s, (zlink_stream_packet_handler_fn)goZlinkStreamPacketTrampoline, (void *)userdata);
}
*/
import "C"

import (
	"runtime/cgo"
	"time"
	"unsafe"
)

// SocketType identifies a raw Core socket pattern.
type SocketType uint32

const (
	SocketTypeAny    SocketType = 0
	SocketTypePair   SocketType = 0x1001
	SocketTypePub    SocketType = 0x1002
	SocketTypeSub    SocketType = 0x1003
	SocketTypeDealer SocketType = 0x1004
	SocketTypeRouter SocketType = 0x1005
	SocketTypeXPub   SocketType = 0x1006
	SocketTypeXSub   SocketType = 0x1007
	SocketTypeStream SocketType = 0x1008
)

type RidDuplicatePolicy int

const (
	RidDuplicateReject   RidDuplicatePolicy = 0
	RidDuplicateHandover RidDuplicatePolicy = 1
)

type SubmitRetryMode int

const (
	SubmitRetryOff          SubmitRetryMode = 0
	SubmitRetryLocalFailure SubmitRetryMode = 1
)

type recvCallback func(*Received)
type sendReadyCallback func()

const recvTopicBufferCap = 64 * 1024

type PairSocket struct {
	*directSocket
}

func newPairSocket(ctx *Context) (*PairSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_PAIR)
	if err != nil {
		return nil, err
	}
	return &PairSocket{
		directSocket: &directSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *PairSocket) Send() SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_send_part(s.raw(), part, C.zlink_send_flags_t(flags), partFlag))
		})
	})
}

type PubSocket struct {
	*publishSocket
}

func newPubSocket(ctx *Context, socketType C.zlink_socket_type_t) (*PubSocket, error) {
	core, err := newSocketCore(ctx, socketType)
	if err != nil {
		return nil, err
	}
	return &PubSocket{
		publishSocket: &publishSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *PubSocket) Publish(topic string) SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return s.withCString(topic, func(cstr *C.char) error {
			return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
				return submitErrorFromResult(C.zlink_publish_part(s.raw(), cstr, part, C.zlink_send_flags_t(flags), partFlag))
			})
		})
	})
}

type SubSocket struct {
	*subscribeSocket
}

func newSubSocket(ctx *Context, socketType C.zlink_socket_type_t) (*SubSocket, error) {
	core, err := newSocketCore(ctx, socketType)
	if err != nil {
		return nil, err
	}
	return &SubSocket{
		subscribeSocket: &subscribeSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *SubSocket) SubscriptionAt(index int) (string, bool, error) {
	return subscriptionAt(s.raw(), index)
}

func (s *SubSocket) TopicsCount() (int, error) {
	return s.connectionSocket.getSubIntOption(C.ZLINK_SUB_OPT_TOPICS_COUNT)
}

type DealerSocket struct {
	*directSocket
}

func newDealerSocket(ctx *Context) (*DealerSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_DEALER)
	if err != nil {
		return nil, err
	}
	return &DealerSocket{
		directSocket: &directSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *DealerSocket) SetRoutingID(id RoutingID) error {
	raw := id.toC()
	return configErrorFromResult(C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size)))
}

func (s *DealerSocket) SetProbe(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	return configErrorFromResult(C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_PROBE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *DealerSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}

func (s *DealerSocket) SetWeight(value int) error {
	raw := C.int(value)
	return configErrorFromResult(C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_WEIGHT, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *DealerSocket) Weight() (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_WEIGHT, unsafe.Pointer(&raw), &size)); err != nil {
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
	return configErrorFromResult(C.zlink_set_dealer_option(s.raw(), C.ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *DealerSocket) Send() SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_send_part(s.raw(), part, C.zlink_send_flags_t(flags), partFlag))
		})
	})
}

func (s *DealerSocket) Request() RequestOp {
	return newRequestBuilder(func(parts []requestBuilderPart, flags SendFlags, timeout time.Duration, callback RequestReplyCallback) error {
		if callback == nil {
			return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		_, err := startDealerRequest(s, flags, timeout, parts, callback)
		if err != nil {
			return err
		}
		return nil
	})
}

type RouterSocket struct {
	*routedSocket
}

func newRouterSocket(ctx *Context) (*RouterSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_ROUTER)
	if err != nil {
		return nil, err
	}
	return &RouterSocket{
		routedSocket: &routedSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *RouterSocket) SetMandatory(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	return configErrorFromResult(C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_MANDATORY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *RouterSocket) SetProbe(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	return configErrorFromResult(C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_PROBE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *RouterSocket) SetRoutingID(id RoutingID) error {
	raw := id.toC()
	return configErrorFromResult(C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size)))
}

func (s *RouterSocket) SetWeight(value int) error {
	raw := C.int(value)
	return configErrorFromResult(C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_WEIGHT, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *RouterSocket) Weight() (int, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_WEIGHT, unsafe.Pointer(&raw), &size)); err != nil {
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
	return configErrorFromResult(C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *RouterSocket) RequestTimeout() (time.Duration, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_router_option(s.raw(), C.ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS, unsafe.Pointer(&raw), &size)); err != nil {
		return 0, err
	}
	return time.Duration(raw) * time.Millisecond, nil
}

func (s *RouterSocket) SetHandover(value bool) error {
	var raw C.int
	if value {
		raw = C.int(C.ZLINK_RID_DUPLICATE_HANDOVER)
	}
	return configErrorFromResult(C.zlink_set_option(s.raw(), C.ZLINK_OPT_RID_DUPLICATE_POLICY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *RouterSocket) SetConnectRoutingID(id RoutingID) error {
	raw := id.toC()
	return configErrorFromResult(C.zlink_set_router_option(s.raw(), C.ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, routingIDPointer(&raw), C.size_t(raw.size)))
}

func (s *RouterSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}

type XPubSocket struct {
	*xpubSubscribeSocket
}

func newXPubSocket(ctx *Context) (*XPubSocket, error) {
	pub, err := newPubSocket(ctx, C.ZLINK_SOCKET_XPUB)
	if err != nil {
		return nil, err
	}
	return &XPubSocket{xpubSubscribeSocket: &xpubSubscribeSocket{publishSocket: pub.publishSocket}}, nil
}

func (s *XPubSocket) Publish(topic string) SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		return s.withCString(topic, func(cstr *C.char) error {
			return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
				return submitErrorFromResult(C.zlink_publish_part(s.raw(), cstr, part, C.zlink_send_flags_t(flags), partFlag))
			})
		})
	})
}

type XSubSocket struct {
	*subscribeSocket
}

func newXSubSocket(ctx *Context) (*XSubSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_XSUB)
	if err != nil {
		return nil, err
	}
	return &XSubSocket{
		subscribeSocket: &subscribeSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *XSubSocket) SubscriptionAt(index int) (string, bool, error) {
	return subscriptionAt(s.raw(), index)
}

func (s *XSubSocket) TopicsCount() (int, error) {
	return s.connectionSocket.getSubIntOption(C.ZLINK_SUB_OPT_TOPICS_COUNT)
}

type StreamSocket struct {
	core *routedSocket
}

func newStreamSocket(ctx *Context) (*StreamSocket, error) {
	core, err := newSocketCore(ctx, C.ZLINK_SOCKET_STREAM)
	if err != nil {
		return nil, err
	}
	return &StreamSocket{
		core: &routedSocket{connectionSocket: &connectionSocket{socketCore: core}},
	}, nil
}

func (s *StreamSocket) raw() unsafe.Pointer {
	if s == nil || s.core == nil {
		return nil
	}
	return s.core.raw()
}

func (s *StreamSocket) Bind(endpoint string) error {
	return s.core.Bind(endpoint)
}

func (s *StreamSocket) Unbind(endpoint string) error {
	return s.core.Unbind(endpoint)
}

func (s *StreamSocket) Close() error {
	if s == nil || s.core == nil {
		return nil
	}
	return s.core.Close()
}

func (s *StreamSocket) SetSendHighWaterMark(value int) error {
	return s.core.SetSendHighWaterMark(value)
}

func (s *StreamSocket) SendHighWaterMark() (int, error) {
	return s.core.SendHighWaterMark()
}

func (s *StreamSocket) SetReceiveHighWaterMark(value int) error {
	return s.core.SetReceiveHighWaterMark(value)
}

func (s *StreamSocket) ReceiveHighWaterMark() (int, error) {
	return s.core.ReceiveHighWaterMark()
}

func (s *StreamSocket) SetLinger(value time.Duration) error {
	return s.core.SetLinger(value)
}

func (s *StreamSocket) SetReceiveTimeout(value time.Duration) error {
	return s.core.SetReceiveTimeout(value)
}

func (s *StreamSocket) SetSendTimeout(value time.Duration) error {
	return s.core.SetSendTimeout(value)
}

func (s *StreamSocket) SetTCPKeepalive(value bool) error {
	return s.core.SetTCPKeepalive(value)
}

func (s *StreamSocket) SetTCPNoDelay(value bool) error {
	return s.core.SetTCPNoDelay(value)
}

func (s *StreamSocket) SetIPv6(value bool) error {
	return s.core.SetIPv6(value)
}

func (s *StreamSocket) LastEndpoint() (string, error) {
	return s.core.LastEndpoint()
}

func (s *StreamSocket) SetTLSServer(certPath string, keyPath string, requireClientCert bool) error {
	return s.core.SetTLSServer(certPath, keyPath, requireClientCert)
}

func (s *StreamSocket) SetTLSClient(caCertPath string, hostname string, trustSystem bool) error {
	return s.core.SetTLSClient(caCertPath, hostname, trustSystem)
}

func (s *StreamSocket) SetRoutingID(id RoutingID) error {
	raw := id.toC()
	return configErrorFromResult(C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size)))
}

func (s *StreamSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}

func (s *StreamSocket) SendTo(target RoutingID) SendOp {
	return newSendBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
		rid := target.toC()
		return submitMultipartFromBuilderParts(parts, func(part *C.zlink_msg_t, partFlag C.zlink_part_flag_t) error {
			return submitErrorFromResult(C.zlink_send_part_rid(s.raw(), &rid, part, C.zlink_send_flags_t(flags), partFlag))
		})
	})
}

// Recv stores the received packet in out so callers can reuse the same
// Received value across recv calls.
func (s *StreamSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	ok, err := (&directSocket{connectionSocket: s.core.connectionSocket}).Recv(out, flags)
	if err != nil || !ok {
		return ok, err
	}
	if out.routingID.Size() > 0 {
		routingID := out.routingID
		out.send = func(sendFlags SendFlags, builderParts []sendBuilderPart) (bool, error) {
			return s.core.submitToBuilder(routingID, sendFlags, builderParts)
		}
	}
	return true, nil
}

func (s *StreamSocket) OnPacket(handler func(RoutingID, *Message, *Message)) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if s == nil || s.core == nil || s.core.isClosed() {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EFAULT)}
	}
	state := newStreamPacketCallbackState(handler)
	handle := cgo.NewHandle(state)
	err := s.core.connectionSocket.replaceCallback(handle, &s.core.streamPacketHandle, nil, func() error {
		return handlerErrorFromResult(C.zlink_stream_packet_handler_go_local(s.raw(), C.uintptr_t(handle)))
	})
	if err != nil {
		state.close()
		handle.Delete()
		return err
	}
	return nil
}

func (s *StreamSocket) SetNotify(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	return configErrorFromResult(C.zlink_set_stream_option(s.raw(), C.ZLINK_STREAM_OPT_NOTIFY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
}

func (s *StreamSocket) Notify() (bool, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_stream_option(s.raw(), C.ZLINK_STREAM_OPT_NOTIFY, unsafe.Pointer(&raw), &size)); err != nil {
		return false, err
	}
	return raw != 0, nil
}

func (s *StreamSocket) OnSendReady(handler func()) error {
	return s.core.connectionSocket.setSendReady(handler)
}
