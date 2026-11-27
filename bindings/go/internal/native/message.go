// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "zlink.h"
_Static_assert(offsetof(zlink_routing_id_t, size) == 0, "routing id size offset");
_Static_assert(offsetof(zlink_routing_id_t, data) == 1, "routing id data offset");
*/
import "C"

import (
	"unsafe"

	"zlink.systems/zlink/contracts"
)

const maxFixedCStringFieldSize = 255

type RoutingID = contracts.RoutingID

var (
	NewRoutingID          = contracts.NewRoutingID
	NewRoutingIDString    = contracts.NewRoutingIDString
	NewRoutingIDUint32    = contracts.NewRoutingIDUint32
	NewRoutingIDUUIDBytes = contracts.NewRoutingIDUUIDBytes
	NewRoutingIDFromHex   = contracts.NewRoutingIDFromHex
)

// RoutingID and zlink_routing_id_t have the same fixed layout. These bounds
// make the FFI conversion a value copy without allocating on send or receive.
var _ [int(unsafe.Sizeof(RoutingID{})) - int(C.sizeof_zlink_routing_id_t)]byte
var _ [int(C.sizeof_zlink_routing_id_t) - int(unsafe.Sizeof(RoutingID{}))]byte

func routingIDToC(r RoutingID) C.zlink_routing_id_t {
	return *(*C.zlink_routing_id_t)(unsafe.Pointer(&r))
}

func routingIDFromC(raw C.zlink_routing_id_t) RoutingID {
	return *(*RoutingID)(unsafe.Pointer(&raw))
}

func routingIDFromCPtr(raw *C.zlink_routing_id_t) RoutingID {
	if raw == nil {
		return RoutingID{}
	}
	return routingIDFromC(*raw)
}

type Message struct {
	msg    C.zlink_msg_t
	closed bool
}

func NewMessage(data []byte) (*Message, error) {
	m := &Message{}
	rc21, errno21 := C.zlink_msg_init_size(&m.msg, C.size_t(len(data)))
	if err := configErrorFromCall(rc21, errno21); err != nil {
		return nil, err
	}
	if len(data) > 0 {
		ptr := C.zlink_msg_data(&m.msg)
		copy(unsafe.Slice((*byte)(ptr), len(data)), data)
	}
	return m, nil
}

func NewMessageWithSize(size int) (*Message, error) {
	if size < 0 {
		return nil, validationError("message size must be >= 0")
	}
	m := &Message{}
	rc22, errno22 := C.zlink_msg_init_size(&m.msg, C.size_t(size))
	if err := configErrorFromCall(rc22, errno22); err != nil {
		return nil, err
	}
	return m, nil
}

func NewMessageString(value string) (*Message, error) {
	return NewMessage([]byte(value))
}

func (m *Message) clone() (*Message, error) {
	dup := &Message{}
	rc23, errno23 := C.zlink_msg_init(&dup.msg)
	if err := configErrorFromCall(rc23, errno23); err != nil {
		return nil, err
	}
	rc24, errno24 := C.zlink_msg_copy(&dup.msg, &m.msg)
	if err := configErrorFromCall(rc24, errno24); err != nil {
		_ = dup.Close()
		return nil, err
	}
	return dup, nil
}

func (m *Message) Clone() (*Message, error) {
	if m == nil || m.closed {
		return nil, newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	return m.clone()
}

func (m *Message) Copy() (*Message, error) {
	return m.Clone()
}

func (m *Message) Close() error {
	if m == nil || m.closed {
		return nil
	}
	rc25, errno25 := C.zlink_msg_close(&m.msg)
	if err := configErrorFromCall(rc25, errno25); err != nil {
		return err
	}
	m.closed = true
	return nil
}

// Data returns a view of the native message payload.
//
// The returned slice is valid only until the message is closed. Callers that
// need to keep the payload after Close, send it to another goroutine, or store
// it beyond the message lifetime should use Bytes instead.
func (m *Message) Data() []byte {
	if m == nil || m.closed {
		return nil
	}
	size := int(C.zlink_msg_size(&m.msg))
	if size == 0 {
		return nil
	}
	ptr := C.zlink_msg_data(&m.msg)
	return unsafe.Slice((*byte)(ptr), size)
}

// Bytes returns a snapshot copy of the message payload.
func (m *Message) Bytes() []byte {
	data := m.Data()
	if len(data) == 0 {
		return nil
	}
	return append([]byte(nil), data...)
}

func (m *Message) Size() int {
	if m == nil || m.closed {
		return 0
	}
	return int(C.zlink_msg_size(&m.msg))
}

func (m *Message) IsEmpty() bool {
	return m.Size() == 0
}

func (m *Message) CopyTo(destination []byte) (int, error) {
	data := m.Data()
	if len(destination) < len(data) {
		return 0, validationError("destination buffer too small")
	}
	return copy(destination, data), nil
}

func (m *Message) TryCopyTo(destination []byte) bool {
	_, err := m.CopyTo(destination)
	return err == nil
}

func (m *Message) Text() string {
	return string(m.Data())
}

func (m *Message) String() string {
	return m.Text()
}

func (m *Message) RefCount() int {
	if m == nil || m.closed {
		return 0
	}
	var result C.zlink_config_result_t
	count := C.zlink_msg_refcnt(&m.msg, &result)
	if result != 0 {
		return 0
	}
	return int(count)
}

func (m *Message) moved() {
	m.closed = true
}
