// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include "zlink.h"
*/
import "C"

type TopicMessage struct {
	routingID RoutingID
	topic     string
	parts     []*Message
	topicBuf  []byte
}

func (t *TopicMessage) RoutingID() RoutingID {
	if t == nil {
		return RoutingID{}
	}
	return t.routingID
}

func (t *TopicMessage) HasRoutingID() bool {
	return t != nil && t.routingID.Size() > 0
}

func (t *TopicMessage) Topic() string {
	if t == nil {
		return ""
	}
	return t.topic
}

func (t *TopicMessage) Parts() []*Message {
	if t == nil {
		return nil
	}
	return t.parts
}

func (t *TopicMessage) IsSinglePart() bool {
	return t != nil && len(t.parts) == 1
}

func (t *TopicMessage) FirstPart() (*Message, error) {
	if t == nil {
		return nil, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if len(t.parts) == 0 {
		return nil, &RecvError{Result: RecvNotSupported, nativeErrno: int(C.EINVAL)}
	}
	return t.parts[0], nil
}

func (t *TopicMessage) SinglePartOrError() (*Message, error) {
	if t == nil {
		return nil, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if len(t.parts) != 1 {
		return nil, &RecvError{Result: RecvNotSupported, nativeErrno: int(C.EINVAL)}
	}
	return t.parts[0], nil
}

func (t *TopicMessage) Close() error {
	if t == nil {
		return nil
	}
	var first error
	for _, part := range t.parts {
		if part == nil {
			continue
		}
		if err := part.Close(); err != nil && first == nil {
			first = err
		}
	}
	t.routingID = RoutingID{}
	t.topic = ""
	t.parts = nil
	return first
}
