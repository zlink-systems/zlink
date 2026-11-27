// SPDX-License-Identifier: MPL-2.0

package contracts

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"
)

const maxRoutingIDSize = 255

// RoutingID is a fixed-size value type. The data lives inline in a
// [255]byte array (matching zlink_routing_id_t) rather than a heap
// slice, so construction from a recv path or hex string no longer
// allocates and copies are register/stack moves.
type RoutingID struct {
	size uint8
	data [maxRoutingIDSize]byte
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
	if len(value) == 0 || len(value)%2 != 0 || len(value) > maxRoutingIDSize*2 {
		return RoutingID{}
	}
	var rid RoutingID
	n, err := hex.Decode(rid.data[:], []byte(value))
	if err != nil || n == 0 {
		return RoutingID{}
	}
	rid.size = uint8(n)
	return rid
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
