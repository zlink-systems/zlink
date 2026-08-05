// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

type publishSocket struct {
	*connectionSocket
}

func (s *publishSocket) OnSendReady(handler func()) error {
	return s.setSendReady(handler)
}

func (s *publishSocket) SetNoDrop(value bool) error {
	return s.setPubBoolOption(C.ZLINK_PUB_OPT_NODROP, value)
}

func (s *publishSocket) NoDrop() (bool, error) {
	return s.getPubBoolOption(C.ZLINK_PUB_OPT_NODROP)
}

func (s *publishSocket) SetVerbose(value bool) error {
	return s.setPubBoolOption(C.ZLINK_PUB_OPT_VERBOSE, value)
}

func (s *publishSocket) Verbose() (bool, error) {
	return s.getPubBoolOption(C.ZLINK_PUB_OPT_VERBOSE)
}

func (s *publishSocket) SetVerboser(value bool) error {
	return s.setPubBoolOption(C.ZLINK_PUB_OPT_VERBOSER, value)
}

func (s *publishSocket) Verboser() (bool, error) {
	return s.getPubBoolOption(C.ZLINK_PUB_OPT_VERBOSER)
}

func (s *publishSocket) SetManual(value bool) error {
	return s.setPubBoolOption(C.ZLINK_PUB_OPT_MANUAL, value)
}

func (s *publishSocket) Manual() (bool, error) {
	return s.getPubBoolOption(C.ZLINK_PUB_OPT_MANUAL)
}

func (s *publishSocket) SetManualLastValue(value bool) error {
	return s.setPubBoolOption(C.ZLINK_PUB_OPT_MANUAL_LAST_VALUE, value)
}

func (s *publishSocket) ManualLastValue() (bool, error) {
	return s.getPubBoolOption(C.ZLINK_PUB_OPT_MANUAL_LAST_VALUE)
}

func (s *publishSocket) SetWelcomeMessage(message *Message) error {
	if message == nil {
		return s.setPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, nil)
	}
	return s.setPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, message.Data())
}

func (s *publishSocket) WelcomeMessage() (*Message, error) {
	data, err := s.getPubBytesOption(C.ZLINK_PUB_OPT_WELCOME_MSG, 256)
	if err != nil {
		return nil, err
	}
	return NewMessage(data)
}

func (s *publishSocket) ApproveSubscribe(routingID RoutingID) error {
	return s.setPubRoutingIDOption(C.ZLINK_PUB_OPT_APPROVE_SUBSCRIBE, routingID)
}

func (s *publishSocket) RejectSubscribe(routingID RoutingID) error {
	return s.setPubRoutingIDOption(C.ZLINK_PUB_OPT_REJECT_SUBSCRIBE, routingID)
}

func (s *publishSocket) PubOptions() *PubSocketOptions {
	return &PubSocketOptions{socket: s.connectionSocket}
}
