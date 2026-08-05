package zlink_test

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"sync/atomic"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

var endpointCounter uint64

func newContext(t testing.TB) *zlink.Context {
	t.Helper()
	ctx, err := zlink.NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	return ctx
}

func inprocEndpoint(prefix string) string {
	id := atomic.AddUint64(&endpointCounter, 1)
	return fmt.Sprintf("inproc://%s-%d-%d", prefix, os.Getpid(), id)
}

func tcpEndpoint(t testing.TB) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen error: %v", err)
	}
	addr := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatalf("close listener error: %v", err)
	}
	return "tcp://" + addr
}

func newMessage(t testing.TB, data string) *zlink.Message {
	t.Helper()
	msg, err := zlink.NewMessage([]byte(data))
	if err != nil {
		t.Fatalf("NewMessage(%q) error = %v", data, err)
	}
	return msg
}

func waitForMonitorEvent(t testing.TB, mon *zlink.SocketMonitor, timeout time.Duration) *zlink.MonitorEvent {
	t.Helper()
	type result struct {
		event *zlink.MonitorEvent
		err   error
	}
	ch := make(chan result, 1)
	go func() {
		event, err := mon.Recv(0)
		ch <- result{event: event, err: err}
	}()

	select {
	case out := <-ch:
		if out.err != nil {
			t.Fatalf("monitor recv error: %v", out.err)
		}
		return out.event
	case <-time.After(timeout):
		t.Fatalf("timed out waiting for monitor event after %s", timeout)
		return nil
	}
}

func writeStreamPacket(t testing.TB, conn net.Conn, body []byte) {
	t.Helper()
	frame := make([]byte, 6+len(body))
	binary.BigEndian.PutUint16(frame[:2], 0)
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(body)))
	copy(frame[6:], body)
	if _, err := conn.Write(frame); err != nil {
		t.Fatalf("write stream packet error: %v", err)
	}
}

func readStreamPacketBody(t testing.TB, conn net.Conn) []byte {
	t.Helper()
	var prefix [6]byte
	if _, err := io.ReadFull(conn, prefix[:]); err != nil {
		t.Fatalf("read stream packet prefix error: %v", err)
	}
	headerSize := binary.BigEndian.Uint16(prefix[:2])
	bodySize := binary.BigEndian.Uint32(prefix[2:6])
	if headerSize > 0 {
		if _, err := io.CopyN(io.Discard, conn, int64(headerSize)); err != nil {
			t.Fatalf("discard stream packet header error: %v", err)
		}
	}
	body := make([]byte, int(bodySize))
	if _, err := io.ReadFull(conn, body); err != nil {
		t.Fatalf("read stream packet body error: %v", err)
	}
	return body
}

func frameStreamPacketMessage(t testing.TB, header, body *zlink.Message) *zlink.Message {
	t.Helper()
	if header == nil || body == nil {
		t.Fatalf("frameStreamPacketMessage requires non-nil messages")
	}
	headerData := header.Data()
	bodyData := body.Data()
	frame := make([]byte, 6+len(headerData)+len(bodyData))
	binary.BigEndian.PutUint16(frame[:2], uint16(len(headerData)))
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(bodyData)))
	copy(frame[6:], headerData)
	copy(frame[6+len(headerData):], bodyData)
	msg, err := zlink.NewMessage(frame)
	if err != nil {
		t.Fatalf("NewMessage(packet frame) error = %v", err)
	}
	return msg
}
