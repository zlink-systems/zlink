// SPDX-License-Identifier: MPL-2.0

package native

import (
	"context"
	"time"
)

type SendOp interface {
	Message(message *Message) SendSubmitOp
	MoveMessage(message *Message) SendSubmitOp
	Bytes(data []byte) SendSubmitOp
}

type SendSubmitOp interface {
	Message(message *Message) SendSubmitOp
	MoveMessage(message *Message) SendSubmitOp
	Bytes(data []byte) SendSubmitOp
	Flags(flags SendFlags) SendSubmitOp
	Submit(ctx context.Context) (bool, error)
}

// RoutedSendOp builds a DEALER/ROUTER send whose completion waits for exact-
// target admission without occupying the caller goroutine.
type RoutedSendOp interface {
	Message(message *Message) RoutedSendSubmitOp
	MoveMessage(message *Message) RoutedSendSubmitOp
	Bytes(data []byte) RoutedSendSubmitOp
}

// RoutedSendSubmitOp exposes the single completion-channel terminal for a
// managed routed send. The channel yields nil after Core accepts the complete
// record, or one terminal error, and then closes.
type RoutedSendSubmitOp interface {
	Message(message *Message) RoutedSendSubmitOp
	MoveMessage(message *Message) RoutedSendSubmitOp
	Bytes(data []byte) RoutedSendSubmitOp
	Submit(ctx context.Context) <-chan error
}

type RequestOp interface {
	Message(message *Message) RequestSubmitOp
	Bytes(data []byte) RequestSubmitOp
}

type RequestSubmitOp interface {
	Message(message *Message) RequestSubmitOp
	Bytes(data []byte) RequestSubmitOp
	Timeout(timeout time.Duration) RequestSubmitOp
	Submit(ctx context.Context) <-chan RequestReplyCompletion
}

type ReplyOp interface {
	Message(message *Message) ReplySubmitOp
}

type ReplySubmitOp interface {
	Message(message *Message) ReplySubmitOp
	Flags(flags SendFlags) ReplySubmitOp
	Submit(ctx context.Context) error
}

type sendBuilder struct {
	first [1]sendBuilderPart
	parts []sendBuilderPart
	count int
	flags SendFlags
	submitOnce
	submit func(parts []sendBuilderPart, flags SendFlags) error
}

type sendBuilderPart struct {
	message *Message
	data    []byte
	move    bool
	bytes   bool
}

func newSendBuilder(submit func(parts []sendBuilderPart, flags SendFlags) error) SendOp {
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

// append keeps the common single-part submit on the builder itself. A slice is
// required only after the caller adds a second multipart frame.
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

func (b *sendBuilder) Flags(flags SendFlags) SendSubmitOp {
	b.flags = flags
	return b
}

func (b *sendBuilder) Submit(ctx context.Context) (bool, error) {
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
		parts = b.singlePart()
	}
	if err := b.submit(parts, b.flags); err != nil {
		return submitBackpressureAsNotSubmitted(err)
	}
	return true, nil
}

func (b *sendBuilder) singlePart() []sendBuilderPart {
	return b.first[:]
}

type routedSendBuilder struct {
	parts []sendBuilderPart
	submitOnce
	submit func(context.Context, []sendBuilderPart) <-chan error
}

func newRoutedSendBuilder(submit func(context.Context, []sendBuilderPart) <-chan error) RoutedSendOp {
	return &routedSendBuilder{submit: submit}
}

func (b *routedSendBuilder) Message(message *Message) RoutedSendSubmitOp {
	b.parts = append(b.parts, sendBuilderPart{message: message})
	return b
}

func (b *routedSendBuilder) MoveMessage(message *Message) RoutedSendSubmitOp {
	b.parts = append(b.parts, sendBuilderPart{message: message, move: true})
	return b
}

func (b *routedSendBuilder) Bytes(data []byte) RoutedSendSubmitOp {
	b.parts = append(b.parts, sendBuilderPart{data: data, bytes: true})
	return b
}

func (b *routedSendBuilder) Submit(ctx context.Context) <-chan error {
	if len(b.parts) == 0 {
		return completedSend(configInvalidArgumentError())
	}
	if err := b.markSubmitted(); err != nil {
		return completedSend(err)
	}
	return b.submit(ctx, b.parts)
}

type requestBuilderState struct {
	parts   []requestBuilderPart
	timeout time.Duration
	submitOnce
	submit func(context.Context, []requestBuilderPart, time.Duration) <-chan RequestReplyCompletion
}

type requestBuilder struct {
	state *requestBuilderState
}

type requestBuilderPart struct {
	message *Message
	data    []byte
	bytes   bool
}

func newRequestBuilder(submit func(context.Context, []requestBuilderPart, time.Duration) <-chan RequestReplyCompletion) RequestOp {
	return &requestBuilder{state: &requestBuilderState{submit: submit}}
}

func (b *requestBuilder) Message(message *Message) RequestSubmitOp {
	b.state.parts = append(b.state.parts, requestBuilderPart{message: message})
	return b
}

func (b *requestBuilder) Bytes(data []byte) RequestSubmitOp {
	b.state.parts = append(b.state.parts, requestBuilderPart{data: data, bytes: true})
	return b
}

func (b *requestBuilder) Timeout(timeout time.Duration) RequestSubmitOp {
	b.state.timeout = timeout
	return b
}

func (b *requestBuilder) Submit(ctx context.Context) <-chan RequestReplyCompletion {
	if len(b.state.parts) == 0 {
		return completedRequest(configInvalidArgumentError())
	}
	if err := b.state.markSubmitted(); err != nil {
		return completedRequest(err)
	}
	return b.state.submit(ctx, b.state.parts, b.state.timeout)
}

type replyBuilder struct {
	parts []*Message
	flags SendFlags
	submitOnce
	submit func(parts []*Message, flags SendFlags) error
}

func newReplyBuilder(submit func(parts []*Message, flags SendFlags) error) ReplyOp {
	return &replyBuilder{submit: submit}
}

func (b *replyBuilder) Message(message *Message) ReplySubmitOp {
	b.parts = append(b.parts, message)
	return b
}

func (b *replyBuilder) Flags(flags SendFlags) ReplySubmitOp {
	b.flags = flags
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
	return b.submit(b.parts, b.flags)
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

func completedSend(err error) <-chan error {
	result := make(chan error, 1)
	result <- err
	close(result)
	return result
}

func completedRequest(err error) <-chan RequestReplyCompletion {
	result := make(chan RequestReplyCompletion, 1)
	result <- requestCompletionFromError(err)
	close(result)
	return result
}
