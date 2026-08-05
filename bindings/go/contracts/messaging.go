// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/v11/internal/native"

type (
	// RoutingID is an opaque identifier for a messaging peer or route, 1 to 255 bytes.
	RoutingID = impl.RoutingID
	// Message owns a message payload; sending consumes it and Close releases it.
	Message = impl.Message
	// RequestReplyCallback is invoked with a request result and its reply parts, which it owns.
	RequestReplyCallback = impl.RequestReplyCallback
	// RequestReplyCompletion carries the result and reply parts of a completed request.
	RequestReplyCompletion = impl.RequestReplyCompletion
	// Received is a received message envelope: routing metadata, parts, and an optional reply/send context.
	Received = impl.Received
	// TopicMessage is a received publish: its topic and message parts.
	TopicMessage = impl.TopicMessage
	// SubscriptionEvent is a subscriber's subscribe or unsubscribe as observed by an XPUB socket.
	SubscriptionEvent = impl.SubscriptionEvent
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
