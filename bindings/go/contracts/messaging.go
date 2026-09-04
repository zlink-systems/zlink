// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/internal/native"

type (
	// CompletionKind identifies a native completion record kind. Send is ABI-only;
	// Writable tells a managed send to retry its retained packet.
	CompletionKind = impl.CompletionKind
	// RoutingID is an opaque identifier for a messaging peer or route, 1 to 255 bytes.
	RoutingID = impl.RoutingID
	// Message owns a message payload; sending consumes it and Close releases it.
	Message = impl.Message
	// ReplyToken is an opaque ROUTER request reply capability; its zero value is invalid.
	ReplyToken = impl.ReplyToken
	// Received is a received message envelope: routing metadata, parts, and an optional reply/send context.
	Received = impl.Received
	// TopicMessage is a received publish: its topic and message parts.
	TopicMessage = impl.TopicMessage
	// SubscriptionEvent is a subscriber's subscribe or unsubscribe as observed by an XPUB socket.
	SubscriptionEvent = impl.SubscriptionEvent
	// StreamPacket is reusable output storage for one decoded STREAM packet.
	StreamPacket = impl.StreamPacket
)

const (
	// CompletionSend is retained for ABI compatibility and is never emitted.
	CompletionSend = impl.CompletionSend
	// CompletionRequest carries a completed request and its reply or terminal result.
	CompletionRequest = impl.CompletionRequest
	// CompletionWritable carries the exact wait token whose send target became writable.
	CompletionWritable = impl.CompletionWritable
)

var (
	// NewRoutingID creates a routing id from a copy of the given bytes (1 to 255).
	NewRoutingID = impl.NewRoutingID
	// NewRoutingIDString creates a routing id from the UTF-8 bytes of the string.
	NewRoutingIDString = impl.NewRoutingIDString
	// NewRoutingIDUint32 creates a 4-byte big-endian routing id from the value.
	NewRoutingIDUint32 = impl.NewRoutingIDUint32
	// NewRoutingIDUUIDBytes creates a 16-byte routing id from the UUID bytes.
	NewRoutingIDUUIDBytes = impl.NewRoutingIDUUIDBytes
	// NewRoutingIDFromHex creates a routing id by decoding the hex string.
	NewRoutingIDFromHex = impl.NewRoutingIDFromHex
	// NewMessage creates an empty message.
	NewMessage = impl.NewMessage
	// NewMessageString creates a message holding the UTF-8 bytes of the string.
	NewMessageString = impl.NewMessageString
	// NewMessageWithSize allocates a message with the given number of writable payload bytes.
	NewMessageWithSize = impl.NewMessageWithSize
)
