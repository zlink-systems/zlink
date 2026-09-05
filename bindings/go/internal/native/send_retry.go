// SPDX-License-Identifier: MPL-2.0

package native

/*
#include "zlink.h"
*/
import "C"

// sendRetryPayload owns an immutable logical multipart record while a
// DONTWAIT send waits for its WRITABLE token. Core consumes every native part
// passed to an admission attempt, so each attempt is made from a clone of
// owned and the retained record itself remains available for another retry.
type sendRetryPayload struct {
	owned []*Message

	sources []sendBuilderPart
}

func newSendRetryPayload(parts []sendBuilderPart) (*sendRetryPayload, error) {
	if len(parts) == 0 {
		return nil, configInvalidArgumentError()
	}

	payload := &sendRetryPayload{owned: make([]*Message, len(parts)), sources: parts}
	// Retained wrappers have one backing allocation. Keep the existing pointer
	// view so SEND and REQUEST use the same multipart copy/cleanup path.
	storage := make([]Message, len(parts))
	for i, part := range parts {
		owned := &storage[i]
		var err error
		switch {
		case part.bytes:
			err = initNativeMessageFromBytes(&owned.msg, part.data)
		case part.message == nil || part.message.closed:
			err = configInvalidArgumentError()
		default:
			err = configErrorFromResult(C.zlink_msg_init(&owned.msg))
			if err == nil {
				err = configErrorFromResult(C.zlink_msg_copy(&owned.msg, &part.message.msg))
				if err != nil {
					_ = owned.Close()
				}
			}
		}
		if err != nil {
			payload.owned = payload.owned[:i]
			payload.close()
			return nil, err
		}
		payload.owned[i] = owned
	}

	// MoveMessage transfers ownership when Submit starts. Delay the transfer
	// until every part has been validated and retained so a later invalid part
	// cannot consume an earlier one.
	for _, part := range parts {
		if part.move {
			_ = part.message.Close()
		}
	}
	return payload, nil
}

// takeSourceOwnership consumes Message inputs once Core either admits the
// record or returns a valid WRITABLE wait token. Hard initial failures leave
// ordinary Message inputs with the caller; MoveMessage inputs were transferred
// while the retained packet was built.
func (p *sendRetryPayload) takeSourceOwnership() {
	if p == nil {
		return
	}

	for _, part := range p.sources {
		if part.message != nil && !part.move {
			_ = part.message.Close()
		}
	}
	p.sources = nil
}

func (p *sendRetryPayload) close() {
	if p == nil {
		return
	}

	closeMessageSlice(p.owned)
	p.owned = nil
	p.sources = nil
}
