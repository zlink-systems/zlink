// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include "zlink.h"
*/
import "C"

import "context"

type replyTokenOwner struct{ marker byte }

// ReplyToken is an opaque capability for one ROUTER request. Its zero value is
// invalid. Go's native equality and map hashing include both unexported fields.
type ReplyToken struct {
	owner *replyTokenOwner
	value uint64
}

type Received struct {
	routingID RoutingID
	parts     []*Message
	token     ReplyToken
	reply     func([]*Message) error
	send      func(context.Context, []sendBuilderPart) error
}

func (r *Received) RoutingID() RoutingID {
	if r == nil {
		return RoutingID{}
	}
	return r.routingID
}

func (r *Received) HasRoutingID() bool {
	return r != nil && r.routingID.Size() > 0
}

func (r *Received) Parts() []*Message {
	if r == nil {
		return nil
	}
	return r.parts
}

func (r *Received) beginReceive() []*Message {
	if r == nil {
		return nil
	}
	previous := r.parts
	for _, part := range previous {
		if part != nil {
			_ = part.Close()
		}
	}
	r.routingID = RoutingID{}
	r.parts = nil
	r.token = ReplyToken{}
	r.reply = nil
	r.send = nil
	return previous
}

func (r *Received) replace(
	routingID RoutingID,
	parts []*Message,
	token ReplyToken,
	reply func([]*Message) error,
	send func(context.Context, []sendBuilderPart) error,
) {
	r.routingID = routingID
	r.parts = parts
	r.token = token
	r.reply = reply
	r.send = send
}

func (r *Received) ReplyToken() (ReplyToken, bool) {
	if r == nil || r.token.owner == nil || r.token.value == 0 {
		return ReplyToken{}, false
	}
	return r.token, true
}

func (r *Received) IsSinglePart() bool {
	return r != nil && len(r.parts) == 1
}

func (r *Received) FirstPart() (*Message, error) {
	if r == nil {
		return nil, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if len(r.parts) == 0 {
		return nil, &RecvError{Result: RecvNotSupported, nativeErrno: int(C.EINVAL)}
	}
	return r.parts[0], nil
}

func (r *Received) SinglePartOrError() (*Message, error) {
	if r == nil {
		return nil, &RecvError{Result: RecvInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if len(r.parts) != 1 {
		return nil, &RecvError{Result: RecvNotSupported, nativeErrno: int(C.EINVAL)}
	}
	return r.parts[0], nil
}

// Reply captures the routing ID and opaque reply token from this envelope.
func (r *Received) Reply() ReplyOp {
	if r == nil {
		return newReplyBuilder(func([]*Message) error {
			return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
		})
	}
	reply := r.reply
	token := r.token
	return newReplyBuilder(func(parts []*Message) error {
		if reply == nil || token.owner == nil || token.value == 0 {
			return &SubmitError{Result: SubmitInvalidState, nativeErrno: int(C.EBUSY)}
		}
		return reply(parts)
	})
}

// Send returns an operation builder for a regular routed message back to
// the sender of this Received. The source routing ID is encapsulated.
func (r *Received) Send() SendOp {
	if r == nil {
		return newSendBuilder(func(context.Context, []sendBuilderPart) error {
			return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
		})
	}
	send := r.send
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) error {
		if send == nil {
			return &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		return send(ctx, parts)
	})
}

func (r *Received) Close() error {
	if r == nil {
		return nil
	}
	var first error
	for _, part := range r.parts {
		if part == nil {
			continue
		}
		if err := part.Close(); err != nil && first == nil {
			first = err
		}
	}
	r.routingID = RoutingID{}
	r.parts = nil
	r.token = ReplyToken{}
	r.reply = nil
	r.send = nil
	return first
}
