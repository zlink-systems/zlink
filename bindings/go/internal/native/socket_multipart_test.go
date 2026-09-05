// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"syscall"
	"testing"
)

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

func TestClonedMultipartClosedPartPreservesErrorAndEarlierSource(t *testing.T) {
	first, err := NewMessage([]byte("retained"))
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	closed, err := NewMessage([]byte("closed"))
	if err != nil {
		t.Fatal(err)
	}
	if err = closed.Close(); err != nil {
		t.Fatal(err)
	}
	err = submitMultipartFromClones([]*Message{first, closed}, true, nil)
	var configErr *ConfigError
	if !errors.As(err, &configErr) || configErr.Result != ConfigInvalidHandle || !errors.Is(err, syscall.EFAULT) {
		t.Fatalf("closed multipart source error=%v, want InvalidHandle/EFAULT", err)
	}
	if first.closed || string(first.Data()) != "retained" {
		t.Fatal("failed preparation consumed an earlier source")
	}
}
