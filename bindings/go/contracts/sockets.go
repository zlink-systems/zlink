// SPDX-License-Identifier: MPL-2.0

package contracts

import impl "zlink.systems/zlink/internal/native"

type (
	// SocketType identifies a socket's messaging pattern.
	SocketType = impl.SocketType
	// RidDuplicatePolicy determines how a socket reacts to a peer that reuses an existing routing id.
	RidDuplicatePolicy = impl.RidDuplicatePolicy
	// SubmitRetryMode determines whether a failed submit is retried.
	SubmitRetryMode = impl.SubmitRetryMode
	// ReceiveFlowState is the DEALER/ROUTER receive-flow state.
	ReceiveFlowState = impl.ReceiveFlowState
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
	// SendSubmitOp accepts further parts and a terminal that retries the retained packet after its exact WRITABLE token.
	SendSubmitOp = impl.SendSubmitOp
	// RequestOp builds a request; submitting consumes the parts and awaits a reply.
	RequestOp = impl.RequestOp
	// RequestSubmitOp accepts further parts, a timeout, and the reply-result terminal.
	RequestSubmitOp = impl.RequestSubmitOp
	// ReplyOp builds a reply; submitting consumes the parts.
	ReplyOp = impl.ReplyOp
	// ReplySubmitOp accepts further parts and the flag-free synchronous terminal.
	ReplySubmitOp = impl.ReplySubmitOp
	// PublishOp builds a lossy or NODROP publish operation.
	PublishOp = impl.PublishOp
	// PublishSubmitOp retains publish flags and its synchronous submit result.
	PublishSubmitOp = impl.PublishSubmitOp
	// StreamReceiveMode selects RAW or decoded PACKET receive before bind/connect.
	StreamReceiveMode = impl.StreamReceiveMode
	// SendFlags modify publish behavior; DontWait reports back-pressure instead of blocking.
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
	// SubmitRetryLocalFailure retries configured local connection failures; DONTWAIT back-pressure uses WRITABLE tokens instead.
	SubmitRetryLocalFailure = impl.SubmitRetryLocalFailure
	// SendFlagsNone is the default send behavior: block until the message can be queued.
	SendFlagsNone = impl.SendFlagsNone
	// SendFlagsDontWait does not block; reports back-pressure instead of waiting.
	SendFlagsDontWait = impl.SendFlagsDontWait
	// RecvFlagsNone is the default receive behavior: block until a message is available.
	RecvFlagsNone = impl.RecvFlagsNone
	// RecvFlagsDontWait does not block; returns immediately when no message is available.
	RecvFlagsDontWait = impl.RecvFlagsDontWait
	// ReceiveFlowRunning is the default receive-flow state: the affected Application pipe accepts data normally.
	ReceiveFlowRunning = impl.ReceiveFlowRunning
	// ReceiveFlowPaused pauses an affected Application pipe's remote peer without touching byte HWM or transport backpressure.
	ReceiveFlowPaused        = impl.ReceiveFlowPaused
	StreamReceiveUnspecified = impl.StreamReceiveUnspecified
	StreamReceiveRaw         = impl.StreamReceiveRaw
	StreamReceivePacket      = impl.StreamReceivePacket
)
