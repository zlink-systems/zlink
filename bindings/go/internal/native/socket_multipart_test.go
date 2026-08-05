// SPDX-License-Identifier: MPL-2.0

package native

import "testing"

func TestMixedBuilderPreparationRestoresEarlierMovedMessage(t *testing.T) {
	moved, err := NewMessage([]byte("preserve me"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer moved.Close()

	err = submitMultipartFromBuilderParts([]sendBuilderPart{
		{message: moved, move: true},
		{message: nil},
	}, nil)
	if err == nil {
		t.Fatalf("submitMultipartFromBuilderParts() should reject a nil later part")
	}
	if moved.closed {
		t.Fatalf("pre-submit validation should not consume the earlier moved message")
	}
	if got := string(moved.Data()); got != "preserve me" {
		t.Fatalf("restored moved message data = %q, want %q", got, "preserve me")
	}
}

func TestReceivedAndTopicCloseIgnoreNilParts(t *testing.T) {
	if err := (&Received{parts: []*Message{nil}}).Close(); err != nil {
		t.Fatalf("Received.Close() with a nil part = %v", err)
	}
	if err := (&TopicMessage{parts: []*Message{nil}}).Close(); err != nil {
		t.Fatalf("TopicMessage.Close() with a nil part = %v", err)
	}
}
