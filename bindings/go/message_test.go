package zlink_test

import (
	"testing"

	zlink "zlink.systems/zlink/v11"
)

func TestMessageDiagnosticAPI(t *testing.T) {
	msg := newMessage(t, "diagnostic")
	defer msg.Close()

	if got := msg.RefCount(); got < 1 {
		t.Fatalf("RefCount() = %d, want >= 1", got)
	}

}

func TestNewMessageWithSizeExposesWritablePayload(t *testing.T) {
	msg, err := zlink.NewMessageWithSize(3)
	if err != nil {
		t.Fatalf("NewMessageWithSize() error = %v", err)
	}
	defer msg.Close()

	data := msg.Data()
	data[0] = 0x01
	data[1] = 0x02
	data[2] = 0x03

	if got := msg.Bytes(); string(got) != string([]byte{0x01, 0x02, 0x03}) {
		t.Fatalf("Bytes() = %v, want [1 2 3]", got)
	}
	if msg.IsEmpty() {
		t.Fatalf("IsEmpty() = true, want false")
	}
}

func TestMessageCanonicalCopyHelpers(t *testing.T) {
	msg, err := zlink.NewMessageString("copy-payload")
	if err != nil {
		t.Fatalf("NewMessageString() error = %v", err)
	}
	defer msg.Close()

	if got := msg.Text(); got != "copy-payload" {
		t.Fatalf("Text() = %q, want copy-payload", got)
	}
	if got := msg.String(); got != "copy-payload" {
		t.Fatalf("String() = %q, want copy-payload", got)
	}

	bytes := msg.Bytes()
	bytes[0] = 'C'
	if got := msg.Text(); got != "copy-payload" {
		t.Fatalf("Bytes() should return a snapshot, message text = %q", got)
	}

	copyMsg, err := msg.Copy()
	if err != nil {
		t.Fatalf("Copy() error = %v", err)
	}
	defer copyMsg.Close()
	if got := copyMsg.Text(); got != "copy-payload" {
		t.Fatalf("Copy().Text() = %q, want copy-payload", got)
	}

	target := make([]byte, len("copy-payload"))
	written, err := msg.CopyTo(target)
	if err != nil {
		t.Fatalf("CopyTo() error = %v", err)
	}
	if written != len("copy-payload") || string(target) != "copy-payload" {
		t.Fatalf("CopyTo() wrote %d/%q, want %d/copy-payload", written, string(target), len("copy-payload"))
	}
	if !msg.TryCopyTo(make([]byte, len("copy-payload"))) {
		t.Fatalf("TryCopyTo() = false, want true")
	}
	if msg.TryCopyTo(make([]byte, len("copy-payload")-1)) {
		t.Fatalf("TryCopyTo() = true for small destination, want false")
	}
}

func TestMessageBytesSnapshotSurvivesClose(t *testing.T) {
	msg, err := zlink.NewMessageString("snapshot-payload")
	if err != nil {
		t.Fatalf("NewMessageString() error = %v", err)
	}

	view := msg.Data()
	snapshot := msg.Bytes()
	view[0] = 'S'

	if got := string(snapshot); got != "snapshot-payload" {
		t.Fatalf("Bytes() snapshot = %q, want snapshot-payload", got)
	}
	if err := msg.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	if got := msg.Data(); got != nil {
		t.Fatalf("Data() after Close() = %v, want nil", got)
	}
	if got := string(snapshot); got != "snapshot-payload" {
		t.Fatalf("snapshot after Close() = %q, want snapshot-payload", got)
	}
}

func TestMessageDataViewIsBoundToMessageLifetime(t *testing.T) {
	msg, err := zlink.NewMessageWithSize(4)
	if err != nil {
		t.Fatalf("NewMessageWithSize() error = %v", err)
	}

	data := msg.Data()
	copy(data, []byte("view"))
	if got := string(msg.Bytes()); got != "view" {
		t.Fatalf("Bytes() while open = %q, want view", got)
	}
	snapshot := msg.Bytes()

	if err := msg.Close(); err != nil {
		t.Fatalf("Close() error = %v", err)
	}
	if got := msg.Data(); got != nil {
		t.Fatalf("Data() after Close() = %v, want nil", got)
	}
	if got := string(snapshot); got != "view" {
		t.Fatalf("snapshot after Close() = %q, want view", got)
	}
}

func TestMessageEmptyState(t *testing.T) {
	msg, err := zlink.NewMessageWithSize(0)
	if err != nil {
		t.Fatalf("NewMessageWithSize() error = %v", err)
	}
	defer msg.Close()

	if !msg.IsEmpty() {
		t.Fatalf("IsEmpty() = false, want true")
	}
}
