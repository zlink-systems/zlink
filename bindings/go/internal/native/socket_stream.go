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
	"zlink.systems/zlink/contracts"
)

type StreamSocket struct {
	core *routedSocket
}

type StreamReceiveMode = contracts.StreamReceiveMode

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
		return newRecvError(RecvInvalidState, int(C.EBUSY))
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

func (s *StreamSocket) DisconnectRID(peerRID RoutingID) error {
	return s.core.socketCore.DisconnectRID(peerRID)
}

func (s *StreamSocket) Close() error {
	if s == nil || s.core == nil {
		return nil
	}
	return s.core.Close()
}

// CommonOptions returns the shared option facade for this STREAM socket.
func (s *StreamSocket) CommonOptions() *CommonSocketOptions {
	return s.core.CommonOptions()
}

// SetSendHighWaterMark sets the send byte HWM over the full uint64 range.
func (s *StreamSocket) SetSendHighWaterMark(value uint64) error {
	return s.CommonOptions().SetSendHighWaterMark(value)
}

// SendHighWaterMark returns the send byte HWM without narrowing the Core value.
func (s *StreamSocket) SendHighWaterMark() (uint64, error) {
	return s.CommonOptions().SendHighWaterMark()
}

// SetReceiveHighWaterMark sets the receive byte HWM over the full uint64 range.
func (s *StreamSocket) SetReceiveHighWaterMark(value uint64) error {
	return s.CommonOptions().SetReceiveHighWaterMark(value)
}

// ReceiveHighWaterMark returns the receive byte HWM without narrowing the Core value.
func (s *StreamSocket) ReceiveHighWaterMark() (uint64, error) {
	return s.CommonOptions().ReceiveHighWaterMark()
}

func (s *StreamSocket) SetLinger(value time.Duration) error {
	return s.CommonOptions().SetLinger(value)
}

func (s *StreamSocket) SetReceiveTimeout(value time.Duration) error {
	return s.CommonOptions().SetReceiveTimeout(value)
}

func (s *StreamSocket) SetSendTimeout(value time.Duration) error {
	return s.CommonOptions().SetSendTimeout(value)
}

func (s *StreamSocket) SetTCPKeepalive(value bool) error {
	return s.CommonOptions().SetTCPKeepalive(value)
}

func (s *StreamSocket) SetTCPNoDelay(value bool) error {
	return s.CommonOptions().SetTCPNoDelay(value)
}

func (s *StreamSocket) SetIPv6(value bool) error {
	return s.CommonOptions().SetIPv6(value)
}

func (s *StreamSocket) LastEndpoint() (string, error) {
	return s.CommonOptions().LastEndpoint()
}

func (s *StreamSocket) SetReceiveFlowState(value ReceiveFlowState) error {
	return s.CommonOptions().SetReceiveFlowState(value)
}

func (s *StreamSocket) SetTLSServer(certPath string, keyPath string, requireClientCert bool) error {
	return s.core.SetTLSServer(certPath, keyPath, requireClientCert)
}

func (s *StreamSocket) SetTLSClient(caCertPath string, hostname string, trustSystem bool) error {
	return s.core.SetTLSClient(caCertPath, hostname, trustSystem)
}

func (s *StreamSocket) SetRoutingID(id RoutingID) error {
	raw := routingIDToC(id)
	nativeResult16, nativeErr16 := C.zlink_set_routing_id(s.raw(), routingIDPointer(&raw), C.size_t(raw.size))
	return configErrorFromCall(nativeResult16, nativeErr16)
}

func (s *StreamSocket) RoutingID() (RoutingID, error) {
	return getHandleRoutingID(s.raw())
}

func (s *StreamSocket) SendTo(target RoutingID) SendOp {
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) (SendSubmission, error) {
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
	out.source = s.core.socketCore
	return true, nil
}

func (s *StreamSocket) RecvPacket(out *StreamPacket, flags RecvFlags) (bool, error) {
	if out == nil {
		return false, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	if !out.receiving.CompareAndSwap(false, true) {
		return false, newRecvError(RecvInvalidState, int(C.EBUSY))
	}
	defer out.receiving.Store(false)
	_ = out.reset()

	header := &Message{}
	nativeResult17, nativeErr17 := C.zlink_msg_init(&header.msg)
	if err := configErrorFromCall(nativeResult17, nativeErr17); err != nil {
		return false, err
	}
	body := &Message{}
	nativeResult18, nativeErr18 := C.zlink_msg_init(&body.msg)
	if err := configErrorFromCall(nativeResult18, nativeErr18); err != nil {
		_ = header.Close()
		return false, err
	}
	var sourceRID *C.zlink_routing_id_t
	result, cerr := C.zlink_stream_recv_packet(
		s.raw(), &sourceRID, &header.msg, &body.msg, C.zlink_recv_flags_t(flags))
	if result == C.ZLINK_RECV_NO_DATA {
		_ = header.Close()
		_ = body.Close()
		return false, nil
	}
	if err := recvErrorFromCall(result, cerr); err != nil {
		_ = header.Close()
		_ = body.Close()
		return false, err
	}
	routingID := routingIDFromCPtr(sourceRID)
	if routingID.Size() == 0 {
		_ = header.Close()
		_ = body.Close()
		return false, newRecvError(RecvInternalError, int(C.EPROTO))
	}
	out.routingID = routingID
	out.header = header
	out.body = body
	return true, nil
}

func (s *StreamSocket) ReceiveMode() (StreamReceiveMode, error) {
	if s == nil || s.core == nil || s.core.isClosed() {
		return StreamReceiveUnspecified, newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult19, nativeErr19 := C.zlink_get_stream_option(
		s.raw(), C.ZLINK_STREAM_OPT_RECV_MODE, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult19, nativeErr19); err != nil {
		return StreamReceiveUnspecified, err
	}
	return StreamReceiveMode(raw), nil
}

func (s *StreamSocket) SetReceiveMode(mode StreamReceiveMode) error {
	if mode != StreamReceiveRaw && mode != StreamReceivePacket {
		return newConfigError(ConfigInvalidArgument, int(C.EINVAL))
	}
	if s == nil || s.core == nil || s.core.isClosed() {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	raw := C.int(mode)
	nativeResult20, nativeErr20 := C.zlink_set_stream_option(
		s.raw(), C.ZLINK_STREAM_OPT_RECV_MODE, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult20, nativeErr20)
}

func (s *StreamSocket) SetNotify(value bool) error {
	var raw C.int
	if value {
		raw = 1
	}
	nativeResult21, nativeErr21 := C.zlink_set_stream_option(s.raw(), C.ZLINK_STREAM_OPT_NOTIFY, unsafe.Pointer(&raw), C.size_t(C.sizeof_int))
	return configErrorFromCall(nativeResult21, nativeErr21)
}

func (s *StreamSocket) Notify() (bool, error) {
	var raw C.int
	size := C.size_t(C.sizeof_int)
	nativeResult22, nativeErr22 := C.zlink_get_stream_option(s.raw(), C.ZLINK_STREAM_OPT_NOTIFY, unsafe.Pointer(&raw), &size)
	if err := configErrorFromCall(nativeResult22, nativeErr22); err != nil {
		return false, err
	}
	return raw != 0, nil
}
