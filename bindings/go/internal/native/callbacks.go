// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"bytes"
	"log"
	"runtime"
	"runtime/cgo"
	"runtime/debug"
	"strconv"
	"sync"
	"sync/atomic"
	"unsafe"
)

type callbackRegistration interface {
	close()
}

type callbackTask struct {
	label   string
	invoke  func()
	cleanup func()
}

type callbackDispatcher struct {
	mu       sync.Mutex
	cond     *sync.Cond
	queue    []*callbackTask
	head     int
	closed   bool
	done     chan struct{}
	workerID atomic.Uint64
}

func newCallbackDispatcher() *callbackDispatcher {
	dispatcher := &callbackDispatcher{
		done:  make(chan struct{}),
		queue: make([]*callbackTask, 0, 16),
	}
	dispatcher.cond = sync.NewCond(&dispatcher.mu)
	go dispatcher.loop()
	return dispatcher
}

// enqueue appends a task; loop() consumes via the `head` cursor so dequeue
// is O(1) and the per-message copy/shift that previously ran in loop() is
// gone. The cursor is compacted back to zero whenever the queue drains so
// the underlying slice does not grow unbounded.
func (d *callbackDispatcher) enqueue(task *callbackTask) bool {
	if d == nil || task == nil {
		return false
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.closed {
		return false
	}
	d.queue = append(d.queue, task)
	d.cond.Signal()
	return true
}

func (d *callbackDispatcher) close() {
	if d == nil {
		return
	}
	d.mu.Lock()
	if d.closed {
		d.mu.Unlock()
		if d.workerID.Load() == currentGoroutineID() {
			return
		}
		<-d.done
		return
	}
	d.closed = true
	d.cond.Broadcast()
	d.mu.Unlock()
	if d.workerID.Load() == currentGoroutineID() {
		return
	}
	<-d.done
}

func (d *callbackDispatcher) loop() {
	defer close(d.done)
	d.workerID.Store(currentGoroutineID())
	for {
		d.mu.Lock()
		for d.head >= len(d.queue) && !d.closed {
			d.cond.Wait()
		}
		if d.head >= len(d.queue) && d.closed {
			d.mu.Unlock()
			return
		}
		task := d.queue[d.head]
		d.queue[d.head] = nil
		d.head++
		// Reset the cursor when the queue drains; avoids unbounded growth
		// of the underlying slice header. Cheap because head==len here.
		if d.head == len(d.queue) {
			d.head = 0
			d.queue = d.queue[:0]
		}
		d.mu.Unlock()
		task.run()
	}
}

func currentGoroutineID() uint64 {
	var buf [64]byte
	n := runtime.Stack(buf[:], false)
	fields := bytes.Fields(buf[:n])
	if len(fields) < 2 {
		return 0
	}
	id, err := strconv.ParseUint(string(fields[1]), 10, 64)
	if err != nil {
		return 0
	}
	return id
}

func (t *callbackTask) run() {
	if t == nil || t.invoke == nil {
		return
	}
	defer func() {
		if recovered := recover(); recovered != nil {
			if t.cleanup != nil {
				t.cleanup()
			}
			log.Printf("zlink: recovered panic in %s callback: %v\n%s", t.label, recovered, debug.Stack())
		}
	}()
	t.invoke()
}

type recvCallbackState struct {
	dispatcher *callbackDispatcher
	handler    recvCallback
}

type completionControlCallbackState struct {
	dispatcher *callbackDispatcher
	handler    func(*Received)
}

func newCompletionControlCallbackState(handler func(*Received)) *completionControlCallbackState {
	return &completionControlCallbackState{
		dispatcher: newCallbackDispatcher(),
		handler:    handler,
	}
}

func (s *completionControlCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

func newRecvCallbackState(handler recvCallback) *recvCallbackState {
	return &recvCallbackState{
		dispatcher: newCallbackDispatcher(),
		handler:    handler,
	}
}

func (s *recvCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

type sendReadyCallbackState struct {
	dispatcher *callbackDispatcher
	handler    sendReadyCallback
}

func newSendReadyCallbackState(handler sendReadyCallback) *sendReadyCallbackState {
	return &sendReadyCallbackState{
		dispatcher: newCallbackDispatcher(),
		handler:    handler,
	}
}

func (s *sendReadyCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

type monitorCallbackState struct {
	dispatcher *callbackDispatcher
	handler    func(*MonitorEvent)
}

func newMonitorCallbackState(handler func(*MonitorEvent)) *monitorCallbackState {
	return &monitorCallbackState{
		dispatcher: newCallbackDispatcher(),
		handler:    handler,
	}
}

func (s *monitorCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

type streamPacketCallbackState struct {
	dispatcher *callbackDispatcher
	handler    func(RoutingID, *Message, *Message)
}

func newStreamPacketCallbackState(handler func(RoutingID, *Message, *Message)) *streamPacketCallbackState {
	return &streamPacketCallbackState{
		dispatcher: newCallbackDispatcher(),
		handler:    handler,
	}
}

func (s *streamPacketCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

func releaseCallbackHandle(handle cgo.Handle) {
	if handle == 0 {
		return
	}
	if registration, ok := handle.Value().(callbackRegistration); ok {
		registration.close()
	}
	handle.Delete()
}

// safeHandleAs looks up a cgo.Handle and type-asserts it to T in one step.
//
// The previous design returned `value any` from safeHandleValue and forced
// every trampoline to do its own `value.(*X)` assertion immediately after,
// which is on the hot path of every recv/subscribe/send-ready/packet
// callback. Folding the assertion in here removes the extra interface
// variable per call and lets the compiler see the concrete type at the
// call site.
//
// The defer/recover guard is preserved because cgo.Handle.Value panics if
// the handle was already deleted (e.g. socket close racing an in-flight
// callback dispatch); we want a swallowed callback instead of a process
// crash in that window.
func safeHandleAs[T any](userdata C.uintptr_t) (value T, ok bool) {
	defer func() {
		if recover() != nil {
			var zero T
			value = zero
			ok = false
		}
	}()
	raw := cgo.Handle(userdata).Value()
	typed, ok := raw.(T)
	if !ok {
		var zero T
		return zero, false
	}
	return typed, true
}

//export goZlinkRecvTrampoline
func goZlinkRecvTrampoline(sourceRID *C.zlink_routing_id_t, parts *C.zlink_msg_t, partCount C.size_t, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*recvCallbackState](userdata)
	if !ok {
		discardParts(parts, partCount)
		return
	}
	ownedParts, err := takeParts(parts, partCount)
	if err != nil {
		return
	}
	received := &Received{
		routingID: routingIDFromCPtr(sourceRID),
		parts:     ownedParts,
	}
	if state.dispatcher.enqueue(&callbackTask{
		label: "receive",
		invoke: func() {
			state.handler(received)
		},
		cleanup: func() {
			_ = received.Close()
		},
	}) {
		return
	}
	_ = received.Close()
}

//export goZlinkSendReadyTrampoline
func goZlinkSendReadyTrampoline(_ unsafe.Pointer, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*sendReadyCallbackState](userdata)
	if !ok {
		return
	}
	state.dispatcher.enqueue(&callbackTask{
		label: "send-ready",
		invoke: func() {
			state.handler()
		},
	})
}

//export goZlinkStreamPacketTrampoline
func goZlinkStreamPacketTrampoline(_ unsafe.Pointer, sourceRID *C.zlink_routing_id_t, header *C.zlink_msg_t, body *C.zlink_msg_t, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*streamPacketCallbackState](userdata)
	if !ok {
		discardParts(header, 1)
		discardParts(body, 1)
		return
	}
	headerParts, err := takeParts(header, 1)
	if err != nil {
		discardParts(body, 1)
		return
	}
	bodyParts, err := takeParts(body, 1)
	if err != nil {
		MultipartClose(headerParts)
		return
	}
	if len(headerParts) != 1 || len(bodyParts) != 1 {
		MultipartClose(headerParts)
		MultipartClose(bodyParts)
		return
	}
	source := routingIDFromCPtr(sourceRID)
	headerMessage := headerParts[0]
	bodyMessage := bodyParts[0]
	if state.dispatcher.enqueue(&callbackTask{
		label: "stream-packet",
		invoke: func() {
			state.handler(source, headerMessage, bodyMessage)
		},
		cleanup: func() {
			MultipartClose([]*Message{headerMessage, bodyMessage})
		},
	}) {
		return
	}
	MultipartClose(headerParts)
	MultipartClose(bodyParts)
}

//export goZlinkTimerTrampoline
func goZlinkTimerTrampoline(timer unsafe.Pointer, fireCount C.uint64_t, userdata C.uintptr_t) {
	state, ok := safeHandleAs[*timerCallbackState](userdata)
	if !ok {
		return
	}
	state.dispatcher.enqueue(&callbackTask{
		label: "timer-fire",
		invoke: func() {
			state.handler(state.timer, uint64(fireCount))
		},
	})
}
