// SPDX-License-Identifier: MPL-2.0

package contracts

// Subscription is a subscriber socket's filter and pattern flag.
type Subscription struct {
	Filter  string
	Pattern bool
}

type SendFlags int

const (
	SendFlagsNone     SendFlags = 0
	SendFlagsDontWait SendFlags = 1
)

type RecvFlags int

const (
	RecvFlagsNone     RecvFlags = 0
	RecvFlagsDontWait RecvFlags = 1
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

type StreamReceiveMode int32

const (
	StreamReceiveUnspecified StreamReceiveMode = iota
	StreamReceiveRaw
	StreamReceivePacket
)

// RouterRoute is one selected ROUTER route: the peer routing id and the
// opaque nonzero generation of the route Core selected for it. Compare
// generations only for equality.
type RouterRoute struct {
	RoutingID       RoutingID
	RouteGeneration uint64
}
