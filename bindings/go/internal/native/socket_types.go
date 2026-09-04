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
	"sync/atomic"
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

// ReceiveFlowState is the DEALER/ROUTER receive-flow state. Control uses the
// Application connection for count-1 peers and the Completion connection for
// count-2 ROUTER-ROUTER peers. It mirrors
// zlink_receive_flow_state_t and is an absolute socket-wide state, not a
// counter: setting the current value again succeeds as a no-op.
type ReceiveFlowState int32

const (
	ReceiveFlowRunning ReceiveFlowState = 0
	ReceiveFlowPaused  ReceiveFlowState = 1
)

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
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) error {
		return submitManagedSend(ctx, s.socketCore, nil, parts)
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

func (s *PubSocket) Publish(topic string) PublishOp {
	return newPublishBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
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
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) error {
		return submitManagedSend(ctx, s.socketCore, nil, parts)
	})
}

func (s *DealerSocket) Request() RequestOp {
	return newRequestBuilder(func(ctx context.Context, parts []requestBuilderPart, timeout time.Duration) ([]*Message, error) {
		return submitCompletionRequest(ctx, s.socketCore, nil, timeout, parts)
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

func (s *XPubSocket) Publish(topic string) PublishOp {
	return newPublishBuilder(func(parts []sendBuilderPart, flags SendFlags) error {
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

type StreamReceiveMode int32

const (
	StreamReceiveUnspecified StreamReceiveMode = iota
	StreamReceiveRaw
	StreamReceivePacket
)

// StreamPacket is reusable caller-owned storage for one decoded STREAM
// packet. Its zero value is empty and ready for RecvPacket.
type StreamPacket struct {
	routingID RoutingID
	header    *Message
	body      *Message
	receiving atomic.Bool
}

func (p *StreamPacket) Empty() bool {
	return p == nil || (p.routingID.Size() == 0 && p.header == nil && p.body == nil)
}

func (p *StreamPacket) RoutingID() RoutingID {
	if p == nil {
		return RoutingID{}
	}
	return p.routingID
}

func (p *StreamPacket) HasRoutingID() bool { return p != nil && p.routingID.Size() > 0 }

func (p *StreamPacket) Header() *Message {
	if p == nil {
		return nil
	}
	return p.header
}

func (p *StreamPacket) Body() *Message {
	if p == nil {
		return nil
	}
	return p.body
}

func (p *StreamPacket) reset() error {
	if p == nil {
		return nil
	}
	var first error
	if p.header != nil {
		first = p.header.Close()
	}
	if p.body != nil {
		if err := p.body.Close(); err != nil && first == nil {
			first = err
		}
	}
	p.routingID = RoutingID{}
	p.header = nil
	p.body = nil
	return first
}

func (p *StreamPacket) Close() error {
	if p == nil {
		return nil
	}
	if !p.receiving.CompareAndSwap(false, true) {
		return &RecvError{Result: RecvInvalidState, nativeErrno: int(C.EBUSY)}
	}
	defer p.receiving.Store(false)
	return p.reset()
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

func (s *StreamSocket) completionDrainOwner() *completionOwner {
	if s == nil || s.core == nil {
		return nil
	}
	return s.core.socketCore.completion
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

func (s *StreamSocket) SetReceiveFlowState(value ReceiveFlowState) error {
	return s.core.SetReceiveFlowState(value)
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
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) error {
		return submitManagedSend(ctx, s.core.socketCore, &target, parts)
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
		out.send = func(ctx context.Context, builderParts []sendBuilderPart) error {
			return submitManagedSend(ctx, s.core.socketCore, &routingID, builderParts)
		}
	}
	return true, nil
}

func (s *StreamSocket) RecvPacket(out *StreamPacket, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if !out.receiving.CompareAndSwap(false, true) {
		return false, &RecvError{Result: RecvInvalidState, nativeErrno: int(C.EBUSY)}
	}
	defer out.receiving.Store(false)
	_ = out.reset()

	header := &Message{}
	if err := configErrorFromResult(C.zlink_msg_init(&header.msg)); err != nil {
		return false, err
	}
	body := &Message{}
	if err := configErrorFromResult(C.zlink_msg_init(&body.msg)); err != nil {
		_ = header.Close()
		return false, err
	}
	var sourceRID *C.zlink_routing_id_t
	result := C.zlink_stream_recv_packet(
		s.raw(), &sourceRID, &header.msg, &body.msg, C.zlink_recv_flags_t(flags))
	if result == C.ZLINK_RECV_NO_DATA {
		_ = header.Close()
		_ = body.Close()
		return false, nil
	}
	if err := recvErrorFromResult(result); err != nil {
		_ = header.Close()
		_ = body.Close()
		return false, err
	}
	routingID := routingIDFromCPtr(sourceRID)
	if routingID.Size() == 0 {
		_ = header.Close()
		_ = body.Close()
		return false, &RecvError{Result: RecvInternalError, nativeErrno: int(C.EPROTO)}
	}
	out.routingID = routingID
	out.header = header
	out.body = body
	return true, nil
}

func (s *StreamSocket) ReceiveMode() (StreamReceiveMode, error) {
	if s == nil || s.core == nil || s.core.isClosed() {
		return StreamReceiveUnspecified, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.int
	size := C.size_t(C.sizeof_int)
	if err := configErrorFromResult(C.zlink_get_stream_option(
		s.raw(), C.ZLINK_STREAM_OPT_RECV_MODE, unsafe.Pointer(&raw), &size)); err != nil {
		return StreamReceiveUnspecified, err
	}
	return StreamReceiveMode(raw), nil
}

func (s *StreamSocket) SetReceiveMode(mode StreamReceiveMode) error {
	if mode != StreamReceiveRaw && mode != StreamReceivePacket {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if s == nil || s.core == nil || s.core.isClosed() {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	raw := C.int(mode)
	return configErrorFromResult(C.zlink_set_stream_option(
		s.raw(), C.ZLINK_STREAM_OPT_RECV_MODE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int)))
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
