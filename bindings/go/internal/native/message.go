// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdlib.h>
#include <string.h>
#include "zlink.h"
*/
import "C"

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"
	"unsafe"
)

const maxRoutingIDSize = 255
const maxFixedCStringFieldSize = 255

// RoutingID is a fixed-size value type. The data lives inline in a
// [255]byte array (matching zlink_routing_id_t) rather than a heap
// slice, so construction from a recv path or hex string no longer
// allocates and copies are register/stack moves.
type RoutingID struct {
	data [maxRoutingIDSize]byte
	size uint8
}

func NewRoutingID(data []byte) RoutingID {
	if len(data) == 0 || len(data) > maxRoutingIDSize {
		return RoutingID{}
	}
	var rid RoutingID
	rid.size = uint8(copy(rid.data[:], data))
	return rid
}

// NewRoutingIDString encodes a user-visible routing id string as UTF-8.
// Invalid input returns the empty RoutingID value.
func NewRoutingIDString(value string) RoutingID {
	return NewRoutingID([]byte(value))
}

func NewRoutingIDUint32(value uint32) RoutingID {
	var data [4]byte
	binary.BigEndian.PutUint32(data[:], value)
	return NewRoutingID(data[:])
}

func NewRoutingIDUUIDBytes(value [16]byte) RoutingID {
	return NewRoutingID(value[:])
}

// NewRoutingIDFromHex parses the hex form returned by RoutingID.Hex. Invalid
// input returns the empty RoutingID value.
func NewRoutingIDFromHex(value string) RoutingID {
	rid, err := parseRoutingIDHex(value)
	if err != nil {
		return RoutingID{}
	}
	return rid
}

// parseRoutingIDHex parses the hex form returned by RoutingID.Hex. Invalid
// input returns *ConfigError.
func parseRoutingIDHex(value string) (RoutingID, error) {
	if len(value) == 0 || len(value)%2 != 0 || len(value) > maxRoutingIDSize*2 {
		return RoutingID{}, validationError("routing id string must be non-empty even-length hex and decode to at most %d bytes", maxRoutingIDSize)
	}
	data, err := hex.DecodeString(value)
	if err != nil {
		return RoutingID{}, validationError("routing id string must contain only hex digits")
	}
	rid := NewRoutingID(data)
	if rid.Size() == 0 {
		return RoutingID{}, validationError("routing id length must be between 1 and %d bytes", maxRoutingIDSize)
	}
	return rid, nil
}

func (r RoutingID) Bytes() []byte {
	return append([]byte(nil), r.data[:r.size]...)
}

func (r RoutingID) Size() int {
	return int(r.size)
}

func (r RoutingID) Hash() uint64 {
	const (
		offset64 = 14695981039346656037
		prime64  = 1099511628211
	)
	hash := uint64(offset64)
	for _, b := range r.data[:r.size] {
		hash ^= uint64(b)
		hash *= prime64
	}
	return hash
}

func (r RoutingID) String() string {
	if text := r.printableUTF8(); text != "" {
		return text
	}
	if r.size == 4 {
		return fmt.Sprintf("%d", binary.BigEndian.Uint32(r.data[:4]))
	}
	if r.size == 16 {
		hexText := r.Hex()
		return fmt.Sprintf("%s-%s-%s-%s-%s", hexText[:8], hexText[8:12], hexText[12:16], hexText[16:20], hexText[20:])
	}
	return "hex:" + r.Hex()
}

func (r RoutingID) Hex() string {
	return hex.EncodeToString(r.data[:r.size])
}

func (r RoutingID) Equal(other RoutingID) bool {
	return bytes.Equal(r.data[:r.size], other.data[:other.size])
}

func (r RoutingID) printableUTF8() string {
	raw := r.data[:r.size]
	if !utf8.Valid(raw) {
		return ""
	}
	text := string(raw)
	if strings.IndexFunc(text, unicode.IsControl) >= 0 {
		return ""
	}
	return text
}

func (r RoutingID) toC() C.zlink_routing_id_t {
	var out C.zlink_routing_id_t
	out.size = C.uint8_t(r.size)
	if r.size > 0 {
		C.memcpy(unsafe.Pointer(&out.data[0]), unsafe.Pointer(&r.data[0]), C.size_t(r.size))
	}
	return out
}

func routingIDFromC(raw C.zlink_routing_id_t) RoutingID {
	size := int(raw.size)
	if size == 0 {
		return RoutingID{}
	}
	var rid RoutingID
	rid.size = uint8(copy(rid.data[:], unsafe.Slice((*byte)(unsafe.Pointer(&raw.data[0])), size)))
	return rid
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
	if err := configErrorFromResult(ConfigResult(C.zlink_msg_init_size(&m.msg, C.size_t(len(data))))); err != nil {
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
	if err := configErrorFromResult(ConfigResult(C.zlink_msg_init_size(&m.msg, C.size_t(size)))); err != nil {
		return nil, err
	}
	return m, nil
}

func NewMessageString(value string) (*Message, error) {
	return NewMessage([]byte(value))
}

func (m *Message) clone() (*Message, error) {
	dup := &Message{}
	if err := configErrorFromResult(ConfigResult(C.zlink_msg_init(&dup.msg))); err != nil {
		return nil, err
	}
	if err := configErrorFromResult(ConfigResult(C.zlink_msg_copy(&dup.msg, &m.msg))); err != nil {
		_ = dup.Close()
		return nil, err
	}
	return dup, nil
}

func (m *Message) Clone() (*Message, error) {
	if m == nil || m.closed {
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
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
	if err := configErrorFromResult(ConfigResult(C.zlink_msg_close(&m.msg))); err != nil {
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
