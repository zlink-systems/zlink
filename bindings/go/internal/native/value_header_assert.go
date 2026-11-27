// SPDX-License-Identifier: MPL-2.0

package native

/*
#include "zlink.h"
*/
import "C"

import "zlink.systems/zlink/contracts"

// The public Go values must remain identical to the Core C header values.
var _ [int(C.ZLINK_MONITOR_STATE_READY) - (1 << 0)]byte
var _ [(1 << 0) - int(C.ZLINK_MONITOR_STATE_READY)]byte
var _ [int(C.ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE) - (1 << 5)]byte
var _ [(1 << 5) - int(C.ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE)]byte

var _ [int(C.ZLINK_AUTO_HWM_PROFILE_COMPACT) - int(contracts.AutoHwmProfileCompact)]byte
var _ [int(contracts.AutoHwmProfileCompact) - int(C.ZLINK_AUTO_HWM_PROFILE_COMPACT)]byte
var _ [int(C.ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY) - int(contracts.AutoHwmProfileLowLatency)]byte
var _ [int(contracts.AutoHwmProfileLowLatency) - int(C.ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY)]byte
var _ [int(C.ZLINK_AUTO_HWM_PROFILE_BALANCED) - int(contracts.AutoHwmProfileBalanced)]byte
var _ [int(contracts.AutoHwmProfileBalanced) - int(C.ZLINK_AUTO_HWM_PROFILE_BALANCED)]byte
var _ [int(C.ZLINK_AUTO_HWM_PROFILE_THROUGHPUT) - int(contracts.AutoHwmProfileThroughput)]byte
var _ [int(contracts.AutoHwmProfileThroughput) - int(C.ZLINK_AUTO_HWM_PROFILE_THROUGHPUT)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_NONE) - int(contracts.AutoHwmRecalcReasonNone)]byte
var _ [int(contracts.AutoHwmRecalcReasonNone) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_NONE)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_INITIAL) - int(contracts.AutoHwmRecalcReasonInitial)]byte
var _ [int(contracts.AutoHwmRecalcReasonInitial) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_INITIAL)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE) - int(contracts.AutoHwmRecalcReasonRoleChange)]byte
var _ [int(contracts.AutoHwmRecalcReasonRoleChange) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE) - int(contracts.AutoHwmRecalcReasonPolicyToggle)]byte
var _ [int(contracts.AutoHwmRecalcReasonPolicyToggle) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_REFRESH) - int(contracts.AutoHwmRecalcReasonRefresh)]byte
var _ [int(contracts.AutoHwmRecalcReasonRefresh) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_REFRESH)]byte
var _ [int(C.ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK) - int(contracts.AutoHwmRecalcReasonDeferredShrink)]byte
var _ [int(contracts.AutoHwmRecalcReasonDeferredShrink) - int(C.ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK)]byte
var _ [int(C.ZLINK_SOCKET_PAIR) - int(contracts.SocketTypePair)]byte
var _ [int(contracts.SocketTypePair) - int(C.ZLINK_SOCKET_PAIR)]byte
var _ [int(C.ZLINK_SOCKET_PUB) - int(contracts.SocketTypePub)]byte
var _ [int(contracts.SocketTypePub) - int(C.ZLINK_SOCKET_PUB)]byte
var _ [int(C.ZLINK_SOCKET_SUB) - int(contracts.SocketTypeSub)]byte
var _ [int(contracts.SocketTypeSub) - int(C.ZLINK_SOCKET_SUB)]byte
var _ [int(C.ZLINK_SOCKET_DEALER) - int(contracts.SocketTypeDealer)]byte
var _ [int(contracts.SocketTypeDealer) - int(C.ZLINK_SOCKET_DEALER)]byte
var _ [int(C.ZLINK_SOCKET_ROUTER) - int(contracts.SocketTypeRouter)]byte
var _ [int(contracts.SocketTypeRouter) - int(C.ZLINK_SOCKET_ROUTER)]byte
var _ [int(C.ZLINK_SOCKET_XPUB) - int(contracts.SocketTypeXPub)]byte
var _ [int(contracts.SocketTypeXPub) - int(C.ZLINK_SOCKET_XPUB)]byte
var _ [int(C.ZLINK_SOCKET_XSUB) - int(contracts.SocketTypeXSub)]byte
var _ [int(contracts.SocketTypeXSub) - int(C.ZLINK_SOCKET_XSUB)]byte
var _ [int(C.ZLINK_SOCKET_STREAM) - int(contracts.SocketTypeStream)]byte
var _ [int(contracts.SocketTypeStream) - int(C.ZLINK_SOCKET_STREAM)]byte
var _ [int(C.ZLINK_RID_DUPLICATE_REJECT) - int(contracts.RidDuplicateReject)]byte
var _ [int(contracts.RidDuplicateReject) - int(C.ZLINK_RID_DUPLICATE_REJECT)]byte
var _ [int(C.ZLINK_RID_DUPLICATE_HANDOVER) - int(contracts.RidDuplicateHandover)]byte
var _ [int(contracts.RidDuplicateHandover) - int(C.ZLINK_RID_DUPLICATE_HANDOVER)]byte
var _ [int(C.ZLINK_SUBMIT_RETRY_OFF) - int(contracts.SubmitRetryOff)]byte
var _ [int(contracts.SubmitRetryOff) - int(C.ZLINK_SUBMIT_RETRY_OFF)]byte
var _ [int(C.ZLINK_SUBMIT_RETRY_LOCAL_FAILURE) - int(contracts.SubmitRetryLocalFailure)]byte
var _ [int(contracts.SubmitRetryLocalFailure) - int(C.ZLINK_SUBMIT_RETRY_LOCAL_FAILURE)]byte
var _ [int(C.ZLINK_RECEIVE_FLOW_RUNNING) - int(contracts.ReceiveFlowRunning)]byte
var _ [int(contracts.ReceiveFlowRunning) - int(C.ZLINK_RECEIVE_FLOW_RUNNING)]byte
var _ [int(C.ZLINK_RECEIVE_FLOW_PAUSED) - int(contracts.ReceiveFlowPaused)]byte
var _ [int(contracts.ReceiveFlowPaused) - int(C.ZLINK_RECEIVE_FLOW_PAUSED)]byte
var _ [int(C.ZLINK_STREAM_RECV_MODE_UNSPECIFIED) - int(contracts.StreamReceiveUnspecified)]byte
var _ [int(contracts.StreamReceiveUnspecified) - int(C.ZLINK_STREAM_RECV_MODE_UNSPECIFIED)]byte
var _ [int(C.ZLINK_STREAM_RECV_MODE_RAW) - int(contracts.StreamReceiveRaw)]byte
var _ [int(contracts.StreamReceiveRaw) - int(C.ZLINK_STREAM_RECV_MODE_RAW)]byte
var _ [int(C.ZLINK_STREAM_RECV_MODE_PACKET) - int(contracts.StreamReceivePacket)]byte
var _ [int(contracts.StreamReceivePacket) - int(C.ZLINK_STREAM_RECV_MODE_PACKET)]byte
var _ [int(C.ZLINK_SEND_FLAGS_NONE) - int(contracts.SendFlagsNone)]byte
var _ [int(contracts.SendFlagsNone) - int(C.ZLINK_SEND_FLAGS_NONE)]byte
var _ [int(C.ZLINK_SEND_FLAGS_DONTWAIT) - int(contracts.SendFlagsDontWait)]byte
var _ [int(contracts.SendFlagsDontWait) - int(C.ZLINK_SEND_FLAGS_DONTWAIT)]byte
var _ [int(C.ZLINK_RECV_FLAGS_NONE) - int(contracts.RecvFlagsNone)]byte
var _ [int(contracts.RecvFlagsNone) - int(C.ZLINK_RECV_FLAGS_NONE)]byte
var _ [int(C.ZLINK_RECV_FLAGS_DONTWAIT) - int(contracts.RecvFlagsDontWait)]byte
var _ [int(contracts.RecvFlagsDontWait) - int(C.ZLINK_RECV_FLAGS_DONTWAIT)]byte
var _ [int(C.ZLINK_POLLIN) - int(contracts.PollIn)]byte
var _ [int(contracts.PollIn) - int(C.ZLINK_POLLIN)]byte
var _ [int(C.ZLINK_POLLOUT) - int(contracts.PollOut)]byte
var _ [int(contracts.PollOut) - int(C.ZLINK_POLLOUT)]byte
var _ [int(C.ZLINK_POLLERR) - int(contracts.PollErr)]byte
var _ [int(contracts.PollErr) - int(C.ZLINK_POLLERR)]byte
var _ [int(C.ZLINK_POLLPRI) - int(contracts.PollPri)]byte
var _ [int(contracts.PollPri) - int(C.ZLINK_POLLPRI)]byte
var _ [int(C.ZLINK_POLLCOMPLETION) - int(contracts.PollCompletion)]byte
var _ [int(contracts.PollCompletion) - int(C.ZLINK_POLLCOMPLETION)]byte
var _ [int(C.ZLINK_POLLROUTE) - int(contracts.PollRoute)]byte
var _ [int(contracts.PollRoute) - int(C.ZLINK_POLLROUTE)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTED) - int(contracts.MonitorEventConnected)]byte
var _ [int(contracts.MonitorEventConnected) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_DELAYED) - int(contracts.MonitorEventConnectDelayed)]byte
var _ [int(contracts.MonitorEventConnectDelayed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_DELAYED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_RETRIED) - int(contracts.MonitorEventConnectRetried)]byte
var _ [int(contracts.MonitorEventConnectRetried) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECT_RETRIED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_LISTENING) - int(contracts.MonitorEventListening)]byte
var _ [int(contracts.MonitorEventListening) - int(C.ZLINK_SOCKET_MONITOR_EVENT_LISTENING)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_BIND_FAILED) - int(contracts.MonitorEventBindFailed)]byte
var _ [int(contracts.MonitorEventBindFailed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_BIND_FAILED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED) - int(contracts.MonitorEventAccepted)]byte
var _ [int(contracts.MonitorEventAccepted) - int(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPT_FAILED) - int(contracts.MonitorEventAcceptFailed)]byte
var _ [int(contracts.MonitorEventAcceptFailed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_ACCEPT_FAILED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSED) - int(contracts.MonitorEventClosed)]byte
var _ [int(contracts.MonitorEventClosed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSE_FAILED) - int(contracts.MonitorEventCloseFailed)]byte
var _ [int(contracts.MonitorEventCloseFailed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CLOSE_FAILED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_DISCONNECTED) - int(contracts.MonitorEventDisconnected)]byte
var _ [int(contracts.MonitorEventDisconnected) - int(C.ZLINK_SOCKET_MONITOR_EVENT_DISCONNECTED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_MONITOR_STOPPED) - int(contracts.MonitorEventMonitorStopped)]byte
var _ [int(contracts.MonitorEventMonitorStopped) - int(C.ZLINK_SOCKET_MONITOR_EVENT_MONITOR_STOPPED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_NO_DETAIL) - int(contracts.MonitorEventHandshakeFailedNoDetail)]byte
var _ [int(contracts.MonitorEventHandshakeFailedNoDetail) - int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_NO_DETAIL)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY) - int(contracts.MonitorEventConnectionReady)]byte
var _ [int(contracts.MonitorEventConnectionReady) - int(C.ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_PROTOCOL) - int(contracts.MonitorEventHandshakeFailedProtocol)]byte
var _ [int(contracts.MonitorEventHandshakeFailedProtocol) - int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_PROTOCOL)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_AUTH) - int(contracts.MonitorEventHandshakeFailedAuth)]byte
var _ [int(contracts.MonitorEventHandshakeFailedAuth) - int(C.ZLINK_SOCKET_MONITOR_EVENT_HANDSHAKE_FAILED_AUTH)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_PEER_WEIGHT_CHANGED) - int(contracts.MonitorEventPeerWeightChanged)]byte
var _ [int(contracts.MonitorEventPeerWeightChanged) - int(C.ZLINK_SOCKET_MONITOR_EVENT_PEER_WEIGHT_CHANGED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED) - int(contracts.MonitorEventSendFlowPaused)]byte
var _ [int(contracts.MonitorEventSendFlowPaused) - int(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED) - int(contracts.MonitorEventSendFlowResumed)]byte
var _ [int(contracts.MonitorEventSendFlowResumed) - int(C.ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_RESUMED)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE) - int(contracts.MonitorEventFlowStateStale)]byte
var _ [int(contracts.MonitorEventFlowStateStale) - int(C.ZLINK_SOCKET_MONITOR_EVENT_FLOW_STATE_STALE)]byte
var _ [int(C.ZLINK_SOCKET_MONITOR_EVENT_ALL) - int(contracts.MonitorEventAll)]byte
var _ [int(contracts.MonitorEventAll) - int(C.ZLINK_SOCKET_MONITOR_EVENT_ALL)]byte
var _ [int(C.ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE) - int(contracts.MonitorEventFlagConnectionReadyEdge)]byte
var _ [int(contracts.MonitorEventFlagConnectionReadyEdge) - int(C.ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)]byte
var _ [int(C.ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE) - int(contracts.MonitorEventFlagSendFlowWritable)]byte
var _ [int(contracts.MonitorEventFlagSendFlowWritable) - int(C.ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE)]byte
var _ [int(C.ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH) - int(contracts.MonitorEventFlagFlowStateStaleEpoch)]byte
var _ [int(contracts.MonitorEventFlagFlowStateStaleEpoch) - int(C.ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH)]byte
var _ [int(C.ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION) - int(contracts.MonitorTransportLaneApplication)]byte
var _ [int(contracts.MonitorTransportLaneApplication) - int(C.ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION)]byte
var _ [int(C.ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION) - int(contracts.MonitorTransportLaneCompletion)]byte
var _ [int(contracts.MonitorTransportLaneCompletion) - int(C.ZLINK_MONITOR_TRANSPORT_LANE_COMPLETION)]byte
var _ [int(C.ZLINK_MONITOR_SOURCE_SOCKET) - int(contracts.MonitorSourceSocket)]byte
var _ [int(contracts.MonitorSourceSocket) - int(C.ZLINK_MONITOR_SOURCE_SOCKET)]byte
