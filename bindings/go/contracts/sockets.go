// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/v11/internal/native"

type (
	// SocketType identifies a socket's messaging pattern.
	SocketType = impl.SocketType
	// RidDuplicatePolicy determines how a socket reacts to a peer that reuses an existing routing id.
	RidDuplicatePolicy = impl.RidDuplicatePolicy
	// SubmitRetryMode determines whether a failed submit is retried.
	SubmitRetryMode = impl.SubmitRetryMode
	// CommonSocketOptions is the typed facade over the socket options shared by every socket type.
	CommonSocketOptions = impl.CommonSocketOptions
	// PubSocketOptions is the typed facade over PUB/XPUB-specific socket options.
	PubSocketOptions = impl.PubSocketOptions
	// PairSocket is an exclusive one-to-one peering with no routing.
	PairSocket = impl.PairSocket
	// PubSocket is a topic-filtered publisher that drops messages with no matching subscriber.
	PubSocket = impl.PubSocket
	// SubSocket is a receive-only subscriber filtered by its subscriptions.
	SubSocket = impl.SubSocket
	// DealerSocket load-balances sends across its connected peers and can issue routed requests.
	DealerSocket = impl.DealerSocket
	// RouterSocket routes messages to peers addressed by routing id; the request/reply server side.
	RouterSocket = impl.RouterSocket
	// XPubSocket is like PubSocket but also surfaces subscriber subscription events.
	XPubSocket = impl.XPubSocket
	// XSubSocket is a subscriber whose subscriptions are carried as messages.
	XSubSocket = impl.XSubSocket
	// StreamSocket exchanges framed packets with raw TCP peers.
	StreamSocket = impl.StreamSocket
	// SendOp builds a multipart send; submitting consumes the added parts.
	SendOp = impl.SendOp
	// SendSubmitOp accepts further parts, flags, and the terminal submit of a send.
	SendSubmitOp = impl.SendSubmitOp
	// RequestOp builds a request; submitting consumes the parts and awaits a reply.
	RequestOp = impl.RequestOp
	// RequestSubmitOp accepts further parts, timeout, flags, and the terminal submit of a request.
	RequestSubmitOp = impl.RequestSubmitOp
	// RequestCallbackSubmitOp is the callback-submission stage of a request.
	RequestCallbackSubmitOp = impl.RequestCallbackSubmitOp
	// ReplyOp builds a reply; submitting consumes the parts.
	ReplyOp = impl.ReplyOp
	// ReplySubmitOp accepts further parts, flags, and the terminal submit of a reply.
	ReplySubmitOp = impl.ReplySubmitOp
	// SendFlags modify send behavior; DontWait reports back-pressure instead of blocking.
	SendFlags = impl.SendFlags
	// RecvFlags modify receive behavior; DontWait returns instead of blocking when no message is available.
	RecvFlags = impl.RecvFlags
)

const (
	// SocketTypeAny matches any socket type; used as a filter value, not a real type.
	SocketTypeAny    = impl.SocketTypeAny
	SocketTypePair   = impl.SocketTypePair
	SocketTypePub    = impl.SocketTypePub
	SocketTypeSub    = impl.SocketTypeSub
	SocketTypeDealer = impl.SocketTypeDealer
	SocketTypeRouter = impl.SocketTypeRouter
	SocketTypeXPub   = impl.SocketTypeXPub
	SocketTypeXSub   = impl.SocketTypeXSub
	SocketTypeStream = impl.SocketTypeStream
	// RidDuplicateReject rejects the new peer and keeps the existing route.
	RidDuplicateReject = impl.RidDuplicateReject
	// RidDuplicateHandover hands the routing id to the new peer, dropping the previous holder.
	RidDuplicateHandover = impl.RidDuplicateHandover
	// SubmitRetryOff never retries; a failed submit fails immediately.
	SubmitRetryOff = impl.SubmitRetryOff
	// SubmitRetryLocalFailure retries when the submit fails locally, such as under back-pressure.
	SubmitRetryLocalFailure = impl.SubmitRetryLocalFailure
	// SendFlagsNone is the default send behavior: block until the message can be queued.
	SendFlagsNone = impl.SendFlagsNone
	// SendFlagsDontWait does not block; reports back-pressure instead of waiting.
	SendFlagsDontWait = impl.SendFlagsDontWait
	// RecvFlagsNone is the default receive behavior: block until a message is available.
	RecvFlagsNone = impl.RecvFlagsNone
	// RecvFlagsDontWait does not block; returns immediately when no message is available.
	RecvFlagsDontWait = impl.RecvFlagsDontWait
)
