package samplecommon

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"runtime"
	"strings"
	"sync/atomic"
	"time"

	zlink "zlink.systems/zlink/v11"
)

var counter uint64

func Must(err error) {
	if err != nil {
		panic(err)
	}
}

func UniqueTCP(prefix string) string {
	id := atomic.AddUint64(&counter, 1)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	Must(err)
	addr := listener.Addr().(*net.TCPAddr)
	_ = listener.Close()
	_ = prefix
	_ = id
	return fmt.Sprintf("tcp://127.0.0.1:%d", addr.Port)
}

func DialEndpoint(endpoint string) net.Conn {
	addr := strings.TrimPrefix(endpoint, "tcp://")
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	Must(err)
	return conn
}

func WriteStreamPacket(conn net.Conn, body []byte) {
	frame := make([]byte, 6+len(body))
	binary.BigEndian.PutUint16(frame[:2], 0)
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(body)))
	copy(frame[6:], body)
	_, err := conn.Write(frame)
	Must(err)
}

func ReadStreamPacketBody(conn net.Conn) []byte {
	var prefix [6]byte
	_, err := io.ReadFull(conn, prefix[:])
	Must(err)
	headerSize := binary.BigEndian.Uint16(prefix[:2])
	bodySize := binary.BigEndian.Uint32(prefix[2:6])
	if headerSize > 0 {
		_, err := io.CopyN(io.Discard, conn, int64(headerSize))
		Must(err)
	}
	body := make([]byte, int(bodySize))
	_, err = io.ReadFull(conn, body)
	Must(err)
	return body
}

func FrameStreamPacketMessage(header, body *zlink.Message) *zlink.Message {
	if header == nil || body == nil {
		Must(fmt.Errorf("frame stream packet requires non-nil messages"))
	}
	headerData := header.Data()
	bodyData := body.Data()
	frame := make([]byte, 6+len(headerData)+len(bodyData))
	binary.BigEndian.PutUint16(frame[:2], uint16(len(headerData)))
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(bodyData)))
	copy(frame[6:], headerData)
	copy(frame[6+len(headerData):], bodyData)
	msg, err := zlink.NewMessage(frame)
	Must(err)
	return msg
}

func OpenMonitor(socket zlink.SocketTarget) *zlink.SocketMonitor {
	mon, err := zlink.OpenSocketMonitor(socket, zlink.MonitorEventConnectionReady)
	Must(err)
	return mon
}

func WaitConnected(serverMon, clientMon *zlink.SocketMonitor) {
	WaitMonitorEvent(serverMon)
	WaitMonitorEvent(clientMon)
}

func WaitMonitorEvent(mon *zlink.SocketMonitor) *zlink.MonitorEvent {
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
		Must(out.err)
		return out.event
	case <-time.After(5 * time.Second):
		Must(fmt.Errorf("timed out waiting for monitor event"))
		return nil
	}
}

func Message(text string) *zlink.Message {
	msg, err := zlink.NewMessage([]byte(text))
	Must(err)
	return msg
}

func MustStep(step string, err error) {
	if err != nil {
		Must(fmt.Errorf("%s: %w", step, err))
	}
}

func WaitUntil(timeout time.Duration, description string, predicate func() bool) {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if predicate() {
			return
		}
		runtime.Gosched()
	}
	Must(fmt.Errorf("timed out waiting for %s", description))
}
