// SPDX-License-Identifier: MPL-2.0

package native

import "sync"

// sendRetryPayload owns an immutable logical multipart record while a
// DONTWAIT send waits for its WRITABLE token. Core consumes every native part
// passed to an admission attempt, so each attempt is made from a clone of
// owned and the retained record itself remains available for another retry.
type sendRetryPayload struct {
	owned []*Message

	consumeOnOwnership []*Message
	consumeOnce        sync.Once
	closeOnce          sync.Once
}

func newSendRetryPayload(parts []sendBuilderPart) (*sendRetryPayload, error) {
	if len(parts) == 0 {
		return nil, configInvalidArgumentError()
	}

	payload := &sendRetryPayload{owned: make([]*Message, len(parts))}
	moveSources := make([]*Message, 0, len(parts))
	for i, part := range parts {
		var (
			owned *Message
			err   error
		)
		switch {
		case part.bytes:
			owned, err = NewMessage(part.data)
		case part.message == nil || part.message.closed:
			err = configInvalidArgumentError()
		default:
			owned, err = part.message.clone()
			if err == nil {
				if part.move {
					moveSources = append(moveSources, part.message)
				} else {
					payload.consumeOnOwnership = append(payload.consumeOnOwnership, part.message)
				}
			}
		}
		if err != nil {
			closeMessageSlice(payload.owned[:i])
			return nil, err
		}
		payload.owned[i] = owned
	}

	// MoveMessage transfers ownership when Submit starts. Delay the transfer
	// until every part has been validated and retained so a later invalid part
	// cannot consume an earlier one.
	closeMessageSlice(moveSources)
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
	p.consumeOnce.Do(func() {
		closeMessageSlice(p.consumeOnOwnership)
		p.consumeOnOwnership = nil
	})
}

func (p *sendRetryPayload) close() {
	if p == nil {
		return
	}
	p.closeOnce.Do(func() {
		closeMessageSlice(p.owned)
		p.owned = nil
		p.consumeOnOwnership = nil
	})
}
