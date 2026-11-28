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
	routingID       RoutingID
	parts           []*Message
	nativeParts     []C.zlink_msg_t
	token           ReplyToken
	source          *socketCore
	routeGeneration uint64
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
	r.clearRecord()
	return previous
}

func (r *Received) clearRecord() {
	r.routingID = RoutingID{}
	r.parts = nil
	r.token = ReplyToken{}
	r.source = nil
	r.routeGeneration = 0
}

func (r *Received) replace(
	routingID RoutingID,
	parts []*Message,
	token ReplyToken,
	source *socketCore,
	routeGeneration uint64,
) {
	r.routingID = routingID
	r.parts = parts
	r.token = token
	r.source = source
	r.routeGeneration = routeGeneration
}

// RouteGeneration returns the opaque nonzero generation of the ROUTER route
// that delivered this record, or 0 when the record was not received from a
// ROUTER. Compare it only for equality with a RouterRoute.RouteGeneration
// from RoutesSnapshot for the same routing id.
func (r *Received) RouteGeneration() uint64 {
	if r == nil {
		return 0
	}
	return r.routeGeneration
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
		return nil, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	if len(r.parts) == 0 {
		return nil, newRecvError(RecvNotSupported, int(C.EINVAL))
	}
	return r.parts[0], nil
}

func (r *Received) SinglePartOrError() (*Message, error) {
	if r == nil {
		return nil, newRecvError(RecvInvalidHandle, int(C.EFAULT))
	}
	if len(r.parts) != 1 {
		return nil, newRecvError(RecvNotSupported, int(C.EINVAL))
	}
	return r.parts[0], nil
}

// Reply captures the routing ID and opaque reply token from this envelope.
func (r *Received) Reply() ReplyOp {
	if r == nil {
		return newReplyBuilder(func([]*Message) error {
			return newSubmitError(SubmitInvalidHandle, int(C.EFAULT))
		})
	}
	source, rid, token := r.source, r.routingID, r.token
	return newReplyBuilder(func(parts []*Message) error {
		if source == nil || token.owner == nil || token.value == 0 {
			return newSubmitError(SubmitInvalidState, int(C.EBUSY))
		}
		return source.submitReply(rid, token.value, parts...)
	})
}

// Send returns an operation builder for a regular routed message back to
// the sender of this Received. The source routing ID is encapsulated.
func (r *Received) Send() SendOp {
	if r == nil {
		return newSendBuilder(func(context.Context, []sendBuilderPart) (SendSubmission, error) {
			return nil, newSubmitError(SubmitInvalidHandle, int(C.EFAULT))
		})
	}
	source, rid := r.source, r.routingID
	return newSendBuilder(func(ctx context.Context, parts []sendBuilderPart) (SendSubmission, error) {
		if source == nil || rid.Size() == 0 {
			return nil, newSubmitError(SubmitInvalidArgument, int(C.EINVAL))
		}
		return submitManagedSend(ctx, source, &rid, parts)
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
	r.clearRecord()
	return first
}
