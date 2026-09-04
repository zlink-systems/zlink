// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"time"
)

type SendOp interface {
	Message(*Message) SendSubmitOp
	MoveMessage(*Message) SendSubmitOp
	Bytes([]byte) SendSubmitOp
}

type SendSubmitOp interface {
	Message(*Message) SendSubmitOp
	MoveMessage(*Message) SendSubmitOp
	Bytes([]byte) SendSubmitOp
	Submit(context.Context) error
}

type RequestOp interface {
	Message(*Message) RequestSubmitOp
	Bytes([]byte) RequestSubmitOp
}

type RequestSubmitOp interface {
	Message(*Message) RequestSubmitOp
	Bytes([]byte) RequestSubmitOp
	Timeout(time.Duration) RequestSubmitOp
	Submit(context.Context) ([]*Message, error)
}

type ReplyOp interface {
	Message(*Message) ReplySubmitOp
}

type ReplySubmitOp interface {
	Message(*Message) ReplySubmitOp
	Submit(context.Context) error
}

type PublishOp interface {
	Message(*Message) PublishSubmitOp
	MoveMessage(*Message) PublishSubmitOp
	Bytes([]byte) PublishSubmitOp
}

type PublishSubmitOp interface {
	Message(*Message) PublishSubmitOp
	MoveMessage(*Message) PublishSubmitOp
	Bytes([]byte) PublishSubmitOp
	Flags(SendFlags) PublishSubmitOp
	Submit(context.Context) (bool, error)
}

type sendBuilderPart struct {
	message *Message
	data    []byte
	move    bool
	bytes   bool
}

type sendBuilder struct {
	first [1]sendBuilderPart
	parts []sendBuilderPart
	count int
	submitOnce
	submit func(context.Context, []sendBuilderPart) error
}

func newSendBuilder(submit func(context.Context, []sendBuilderPart) error) SendOp {
	return &sendBuilder{submit: submit}
}

func (b *sendBuilder) Message(message *Message) SendSubmitOp {
	b.append(sendBuilderPart{message: message})
	return b
}

func (b *sendBuilder) MoveMessage(message *Message) SendSubmitOp {
	b.append(sendBuilderPart{message: message, move: true})
	return b
}

func (b *sendBuilder) Bytes(data []byte) SendSubmitOp {
	b.append(sendBuilderPart{data: data, bytes: true})
	return b
}

func (b *sendBuilder) append(part sendBuilderPart) {
	if b.count == 0 {
		b.first[0] = part
		b.count = 1
		return
	}
	if b.count == 1 {
		b.parts = append(b.parts, b.first[0])
	}
	b.parts = append(b.parts, part)
	b.count++
}

func (b *sendBuilder) builderParts() []sendBuilderPart {
	if b.count == 1 {
		return b.first[:]
	}
	return b.parts
}

func (b *sendBuilder) Submit(ctx context.Context) error {
	if err := contextError(ctx); err != nil {
		return err
	}
	if b.count == 0 {
		return configInvalidArgumentError()
	}
	if err := b.markSubmitted(); err != nil {
		return err
	}
	return b.submit(ctx, b.builderParts())
}

type publishBuilder struct {
	first [1]sendBuilderPart
	parts []sendBuilderPart
	count int
	flags SendFlags
	submitOnce
	submit func([]sendBuilderPart, SendFlags) error
}

func newPublishBuilder(submit func([]sendBuilderPart, SendFlags) error) PublishOp {
	return &publishBuilder{submit: submit}
}

func (b *publishBuilder) Message(message *Message) PublishSubmitOp {
	b.append(sendBuilderPart{message: message})
	return b
}

func (b *publishBuilder) MoveMessage(message *Message) PublishSubmitOp {
	b.append(sendBuilderPart{message: message, move: true})
	return b
}

func (b *publishBuilder) Bytes(data []byte) PublishSubmitOp {
	b.append(sendBuilderPart{data: data, bytes: true})
	return b
}

func (b *publishBuilder) append(part sendBuilderPart) {
	if b.count == 0 {
		b.first[0] = part
		b.count = 1
		return
	}
	if b.count == 1 {
		b.parts = append(b.parts, b.first[0])
	}
	b.parts = append(b.parts, part)
	b.count++
}

func (b *publishBuilder) Flags(flags SendFlags) PublishSubmitOp {
	b.flags = flags
	return b
}

func (b *publishBuilder) Submit(ctx context.Context) (bool, error) {
	if err := contextError(ctx); err != nil {
		return false, err
	}
	if b.count == 0 {
		return false, configInvalidArgumentError()
	}
	if err := b.markSubmitted(); err != nil {
		return false, err
	}
	parts := b.parts
	if b.count == 1 {
		parts = b.first[:]
	}
	if err := b.submit(parts, b.flags); err != nil {
		return submitBackpressureAsNotSubmitted(err)
	}
	return true, nil
}

// Request and send staging have the same native part representation. Keeping
// this as an alias avoids a conversion allocation on the immediately admitted
// REQUEST path; request builders simply never set move.
type requestBuilderPart = sendBuilderPart

type requestBuilder struct {
	parts   []requestBuilderPart
	timeout time.Duration
	submitOnce
	submit func(context.Context, []requestBuilderPart, time.Duration) ([]*Message, error)
}

func newRequestBuilder(submit func(context.Context, []requestBuilderPart, time.Duration) ([]*Message, error)) RequestOp {
	return &requestBuilder{submit: submit}
}

func (b *requestBuilder) Message(message *Message) RequestSubmitOp {
	b.parts = append(b.parts, requestBuilderPart{message: message})
	return b
}

func (b *requestBuilder) Bytes(data []byte) RequestSubmitOp {
	b.parts = append(b.parts, requestBuilderPart{data: data, bytes: true})
	return b
}

func (b *requestBuilder) Timeout(timeout time.Duration) RequestSubmitOp {
	b.timeout = timeout
	return b
}

func (b *requestBuilder) Submit(ctx context.Context) ([]*Message, error) {
	if err := contextError(ctx); err != nil {
		return nil, err
	}
	if len(b.parts) == 0 || b.timeout < 0 {
		return nil, configInvalidArgumentError()
	}
	if err := b.markSubmitted(); err != nil {
		return nil, err
	}
	return b.submit(ctx, b.parts, b.timeout)
}

type replyBuilder struct {
	parts []*Message
	submitOnce
	submit func([]*Message) error
}

func newReplyBuilder(submit func([]*Message) error) ReplyOp {
	return &replyBuilder{submit: submit}
}

func (b *replyBuilder) Message(message *Message) ReplySubmitOp {
	b.parts = append(b.parts, message)
	return b
}

func (b *replyBuilder) Submit(ctx context.Context) error {
	if err := contextError(ctx); err != nil {
		return err
	}
	if len(b.parts) == 0 {
		return configInvalidArgumentError()
	}
	if err := b.markSubmitted(); err != nil {
		return err
	}
	return b.submit(b.parts)
}

func contextError(ctx context.Context) error {
	if ctx == nil {
		return nil
	}
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
		return nil
	}
}
