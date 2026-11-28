//go:build linux

// SPDX-License-Identifier: MPL-2.0

package native

import (
	"bytes"
	"context"
	"testing"

	"zlink.systems/zlink/internal/native/completiontest"
)

func TestTokenlessBackpressurePreservesMessage(t *testing.T) {
	ctx := context.Background()
	target := NewRoutingIDString("submit-target")
	payload := []byte{0x00, 0x7f, 0xff}
	for _, operation := range []struct {
		name   string
		submit func(*RouterSocket, *Message) error
	}{
		{"send", func(socket *RouterSocket, message *Message) error {
			_, err := socket.SendTo(target).Message(message).Submit(ctx)
			return err
		}},
		{"request", func(socket *RouterSocket, message *Message) error {
			_, err := socket.Request(target).Message(message).Submit(ctx)
			return err
		}},
		{"reply", func(socket *RouterSocket, message *Message) error {
			token := ReplyToken{owner: socket.replyOwner, value: 1}
			return socket.Reply(target, token).Message(message).Submit(ctx)
		}},
	} {
		t.Run(operation.name, func(t *testing.T) {
			socket := newBackpressureTestSocket(t)
			message, err := NewMessage(payload)
			if err != nil {
				t.Fatal(err)
			}
			defer message.Close()

			completiontest.BackpressureOnce(false)
			requireTokenlessBackpressure(t, operation.submit(socket, message))
			if got := message.Size(); got != len(payload) {
				t.Fatalf("size after backpressure = %d, want %d", got, len(payload))
			}
			if got := message.Data(); !bytes.Equal(got, payload) {
				t.Fatalf("data after backpressure = %x, want %x", got, payload)
			}

			submission, err := socket.SendTo(target).Message(message).Submit(ctx)
			if err != nil || submission == nil || submission.Result() != SubmitOK {
				t.Fatalf("resubmitting the same Message = (%v, %v), want OK", submission, err)
			}
			checkBackpressureCleanup(t, socket.completion, "BS")
		})
	}
}
