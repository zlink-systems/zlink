// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "time"

type CommonSocketOptions struct {
	socket *connectionSocket
}

type PubSocketOptions struct {
	socket *connectionSocket
}

func (o *PubSocketOptions) SetNoDrop(value bool) error {
	return o.socket.setPubBoolOption(C.ZLINK_PUB_OPT_NODROP, value)
}

func (o *PubSocketOptions) NoDrop() (bool, error) {
	return o.socket.getPubBoolOption(C.ZLINK_PUB_OPT_NODROP)
}

func (o *PubSocketOptions) SetVerbose(value bool) error {
	return o.socket.setPubBoolOption(C.ZLINK_PUB_OPT_VERBOSE, value)
}

func (o *PubSocketOptions) Verbose() (bool, error) {
	return o.socket.getPubBoolOption(C.ZLINK_PUB_OPT_VERBOSE)
}

func (o *PubSocketOptions) SetVerboser(value bool) error {
	return o.socket.setPubBoolOption(C.ZLINK_PUB_OPT_VERBOSER, value)
}

func (o *PubSocketOptions) Verboser() (bool, error) {
	return o.socket.getPubBoolOption(C.ZLINK_PUB_OPT_VERBOSER)
}

func (o *PubSocketOptions) SetManual(value bool) error {
	return o.socket.setPubBoolOption(C.ZLINK_PUB_OPT_MANUAL, value)
}

func (o *PubSocketOptions) Manual() (bool, error) {
	return o.socket.getPubBoolOption(C.ZLINK_PUB_OPT_MANUAL)
}

func (o *PubSocketOptions) TopicsCount() (int, error) {
	return o.socket.getPubIntOption(C.ZLINK_PUB_OPT_TOPICS_COUNT)
}

func (o *PubSocketOptions) SetManualLastValue(value bool) error {
	return o.socket.setPubBoolOption(C.ZLINK_PUB_OPT_MANUAL_LAST_VALUE, value)
}

func (o *PubSocketOptions) ManualLastValue() (bool, error) {
	return o.socket.getPubBoolOption(C.ZLINK_PUB_OPT_MANUAL_LAST_VALUE)
}

func (o *PubSocketOptions) SetWelcomeMessage(message *Message) error {
	if message == nil {
		return o.socket.setPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, nil)
	}
	return o.socket.setPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, message.Data())
}

func (o *PubSocketOptions) WelcomeMessage() (*Message, error) {
	data, err := o.socket.getPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, 256)
	if err != nil {
		return nil, err
	}
	return NewMessage(data)
}

func (o *PubSocketOptions) ApproveSubscribe(routingID RoutingID) error {
	return o.socket.setPubRoutingIDOption(C.ZLINK_PUB_OPT_APPROVE_SUBSCRIBE, routingID)
}

func (o *PubSocketOptions) RejectSubscribe(routingID RoutingID) error {
	return o.socket.setPubRoutingIDOption(C.ZLINK_PUB_OPT_REJECT_SUBSCRIBE, routingID)
}

func (o *CommonSocketOptions) SetLinger(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_LINGER, value)
}

func (o *CommonSocketOptions) Linger() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_LINGER)
}

func (o *CommonSocketOptions) SetSendHighWaterMark(value int) error {
	return o.socket.setHwmOption(C.ZLINK_OPT_SNDHWM, value)
}

func (o *CommonSocketOptions) SendHighWaterMark() (int, error) {
	return o.socket.getHwmOption(C.ZLINK_OPT_SNDHWM)
}

func (o *CommonSocketOptions) SetReceiveHighWaterMark(value int) error {
	return o.socket.setHwmOption(C.ZLINK_OPT_RCVHWM, value)
}

func (o *CommonSocketOptions) ReceiveHighWaterMark() (int, error) {
	return o.socket.getHwmOption(C.ZLINK_OPT_RCVHWM)
}

func (o *CommonSocketOptions) SetSendTimeout(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_SNDTIMEO, value)
}

func (o *CommonSocketOptions) SendTimeout() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_SNDTIMEO)
}

func (o *CommonSocketOptions) SetReceiveTimeout(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_RCVTIMEO, value)
}

func (o *CommonSocketOptions) ReceiveTimeout() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_RCVTIMEO)
}

func (o *CommonSocketOptions) SetImmediate(value bool) error {
	return o.socket.setBoolOption(C.ZLINK_OPT_IMMEDIATE, value)
}

func (o *CommonSocketOptions) Immediate() (bool, error) {
	return o.socket.getBoolOption(C.ZLINK_OPT_IMMEDIATE)
}

func (o *CommonSocketOptions) SetRidDuplicatePolicy(value RidDuplicatePolicy) error {
	return o.socket.setIntOption(C.ZLINK_OPT_RID_DUPLICATE_POLICY, int32(value))
}

func (o *CommonSocketOptions) RidDuplicatePolicy() (RidDuplicatePolicy, error) {
	value, err := o.socket.getIntOption(C.ZLINK_OPT_RID_DUPLICATE_POLICY)
	return RidDuplicatePolicy(value), err
}

func (o *CommonSocketOptions) SetConnectTimeout(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_CONNECT_TIMEOUT, value)
}

func (o *CommonSocketOptions) ConnectTimeout() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_CONNECT_TIMEOUT)
}

func (o *CommonSocketOptions) SetIPv6(value bool) error {
	return o.socket.setBoolOption(C.ZLINK_OPT_IPV6, value)
}

func (o *CommonSocketOptions) IPv6() (bool, error) {
	return o.socket.getBoolOption(C.ZLINK_OPT_IPV6)
}

func (o *CommonSocketOptions) SetTCPNoDelay(value bool) error {
	return o.socket.setBoolOption(C.ZLINK_OPT_TCP_NODELAY, value)
}

func (o *CommonSocketOptions) TCPNoDelay() (bool, error) {
	return o.socket.getBoolOption(C.ZLINK_OPT_TCP_NODELAY)
}

func (o *CommonSocketOptions) SetTCPKeepalive(value bool) error {
	return o.socket.setBoolOption(C.ZLINK_OPT_TCP_KEEPALIVE, value)
}

func (o *CommonSocketOptions) TCPKeepalive() (bool, error) {
	return o.socket.getBoolOption(C.ZLINK_OPT_TCP_KEEPALIVE)
}

func (o *CommonSocketOptions) SetMaxMessageSize(value int64) error {
	return o.socket.setInt64Option(C.ZLINK_OPT_MAXMSGSIZE, value)
}

func (o *CommonSocketOptions) MaxMessageSize() (int64, error) {
	return o.socket.getInt64Option(C.ZLINK_OPT_MAXMSGSIZE)
}

func (o *CommonSocketOptions) SetBacklog(value int) error {
	return o.socket.setIntOption(C.ZLINK_OPT_BACKLOG, int32(value))
}

func (o *CommonSocketOptions) Backlog() (int, error) {
	value, err := o.socket.getIntOption(C.ZLINK_OPT_BACKLOG)
	return int(value), err
}

func (o *CommonSocketOptions) SetReconnectInterval(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_RECONNECT_IVL, value)
}

func (o *CommonSocketOptions) ReconnectInterval() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_RECONNECT_IVL)
}

func (o *CommonSocketOptions) SetReconnectIntervalMax(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_RECONNECT_IVL_MAX, value)
}

func (o *CommonSocketOptions) ReconnectIntervalMax() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_RECONNECT_IVL_MAX)
}

func (o *CommonSocketOptions) SetSubmitRetryMode(value SubmitRetryMode) error {
	return o.socket.setIntOption(C.ZLINK_OPT_SUBMIT_RETRY_MODE, int32(value))
}

func (o *CommonSocketOptions) SubmitRetryMode() (SubmitRetryMode, error) {
	value, err := o.socket.getIntOption(C.ZLINK_OPT_SUBMIT_RETRY_MODE)
	return SubmitRetryMode(value), err
}

func (o *CommonSocketOptions) SetSubmitRetryTimeout(value time.Duration) error {
	return o.socket.setDurationOption(C.ZLINK_OPT_SUBMIT_RETRY_TIMEOUT, value)
}

func (o *CommonSocketOptions) SubmitRetryTimeout() (time.Duration, error) {
	return o.socket.getDurationOption(C.ZLINK_OPT_SUBMIT_RETRY_TIMEOUT)
}

func (o *CommonSocketOptions) SetSubmitRetryAttempts(value int) error {
	return o.socket.setIntOption(C.ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS, int32(value))
}

func (o *CommonSocketOptions) SubmitRetryAttempts() (int, error) {
	value, err := o.socket.getIntOption(C.ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS)
	return int(value), err
}

func (o *CommonSocketOptions) LastEndpoint() (string, error) {
	return o.socket.LastEndpoint()
}
