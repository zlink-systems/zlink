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

// RoutedSendOp builds a DEALER/ROUTER send. The HWM wait belongs to Core, so
// the terminal is the synchronous Submit(ctx) below.
type RoutedSendOp interface {
	Message(message *Message) RoutedSendSubmitOp
	MoveMessage(message *Message) RoutedSendSubmitOp
	Bytes(data []byte) RoutedSendSubmitOp
}

// RoutedSendSubmitOp exposes the single terminal for a managed routed send.
// Submit is synchronous — blocking the calling goroutine is Go's idiomatic
// await, and the wait itself happens inside Core (bounded by the socket's
// SNDTIMEO). ctx owns cancellation and deadlines at the submit boundary:
// an already-cancelled ctx fails before anything reaches the wire.
type RoutedSendSubmitOp interface {
	Message(message *Message) RoutedSendSubmitOp
	MoveMessage(message *Message) RoutedSendSubmitOp
	Bytes(data []byte) RoutedSendSubmitOp
	Flags(flags SendFlags) RoutedSendSubmitOp
	Submit(ctx context.Context) error
}

type RequestOp interface {
	Message(message *Message) RequestSubmitOp
	Bytes(data []byte) RequestSubmitOp
}

type RequestSubmitOp interface {
	Message(message *Message) RequestSubmitOp
	Bytes(data []byte) RequestSubmitOp
	Timeout(timeout time.Duration) RequestSubmitOp
	Flags(flags SendFlags) RequestSyncSubmitOp
	Submit(ctx context.Context) <-chan RequestReplyCompletion
}

// RequestSyncSubmitOp is the synchronous admission terminal for a request.
// Submit returns after Core admits the request (or rejects it immediately with
// SendFlagsDontWait); the reply is still delivered through the completion
// channel so callers can submit continuously and collect later.
type RequestSyncSubmitOp interface {
	Message(message *Message) RequestSyncSubmitOp
	Bytes(data []byte) RequestSyncSubmitOp
	Timeout(timeout time.Duration) RequestSyncSubmitOp
	Flags(flags SendFlags) RequestSyncSubmitOp
	Submit(ctx context.Context) (<-chan RequestReplyCompletion, error)
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
	flags SendFlags
	submitOnce
	submit func(context.Context, SendFlags, []sendBuilderPart) error
}

func newRoutedSendBuilder(submit func(context.Context, SendFlags, []sendBuilderPart) error) RoutedSendOp {
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

func (b *routedSendBuilder) Flags(flags SendFlags) RoutedSendSubmitOp {
	b.flags = flags
	return b
}

func (b *routedSendBuilder) Submit(ctx context.Context) error {
	if len(b.parts) == 0 {
		return configInvalidArgumentError()
	}
	if err := b.markSubmitted(); err != nil {
		return err
	}
	return b.submit(ctx, b.flags, b.parts)
}

type requestBuilderState struct {
	parts   []requestBuilderPart
	timeout time.Duration
	flags   SendFlags
	submitOnce
	submit func(context.Context, SendFlags, []requestBuilderPart, time.Duration) (<-chan RequestReplyCompletion, error)
}

type requestBuilder struct {
	state *requestBuilderState
}

type requestSyncBuilder struct {
	state *requestBuilderState
}

type requestBuilderPart struct {
	message *Message
	data    []byte
	bytes   bool
}

func newRequestBuilder(submit func(context.Context, SendFlags, []requestBuilderPart, time.Duration) (<-chan RequestReplyCompletion, error)) RequestOp {
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

func (b *requestBuilder) Flags(flags SendFlags) RequestSyncSubmitOp {
	b.state.flags = flags
	return &requestSyncBuilder{state: b.state}
}

func (b *requestBuilder) submit(ctx context.Context) (<-chan RequestReplyCompletion, error) {
	if err := contextError(ctx); err != nil {
		return nil, err
	}
	if len(b.state.parts) == 0 {
		return nil, configInvalidArgumentError()
	}
	if err := b.state.markSubmitted(); err != nil {
		return nil, err
	}
	return b.state.submit(ctx, b.state.flags, b.state.parts, b.state.timeout)
}

func (b *requestBuilder) Submit(ctx context.Context) <-chan RequestReplyCompletion {
	completion, err := b.submit(ctx)
	if err != nil {
		return completedRequest(err)
	}
	return completion
}

func (b *requestSyncBuilder) Message(message *Message) RequestSyncSubmitOp {
	b.state.parts = append(b.state.parts, requestBuilderPart{message: message})
	return b
}

func (b *requestSyncBuilder) Bytes(data []byte) RequestSyncSubmitOp {
	b.state.parts = append(b.state.parts, requestBuilderPart{data: data, bytes: true})
	return b
}

func (b *requestSyncBuilder) Timeout(timeout time.Duration) RequestSyncSubmitOp {
	b.state.timeout = timeout
	return b
}

func (b *requestSyncBuilder) Flags(flags SendFlags) RequestSyncSubmitOp {
	b.state.flags = flags
	return b
}

func (b *requestSyncBuilder) Submit(ctx context.Context) (<-chan RequestReplyCompletion, error) {
	return (&requestBuilder{state: b.state}).submit(ctx)
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

func completedRequest(err error) <-chan RequestReplyCompletion {
	result := make(chan RequestReplyCompletion, 1)
	result <- requestCompletionFromError(err)
	close(result)
	return result
}
