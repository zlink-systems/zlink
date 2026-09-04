// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"context"
	goruntime "runtime"
	"runtime/cgo"
	"sync"
	"unsafe"
)

type completionOperationKind uint8

const (
	completionSendRetry completionOperationKind = iota + 1
	completionRequest
)

// completionEntry is the two-phase join between the native submit return and
// a completion captured by whichever drain owner currently owns the socket.
// The cgo handle is only an opaque lookup key; Core never dereferences it.
type completionEntry struct {
	kind completionOperationKind
	send *sendRetryState

	mu          sync.Mutex
	published   bool
	captured    bool
	settled     bool
	publicDone  bool
	completion  uint64
	parts       []*Message
	err         error
	done        chan struct{}
	settledDone chan struct{}
	stopCancel  func() bool
	attemptMu   sync.Mutex
	owner       *completionOwner
	sendWaiting bool

	handle     cgo.Handle
	handleKey  uintptr
	deleteOnce sync.Once
}

func newCompletionEntry(kind completionOperationKind, ctx context.Context) *completionEntry {
	entry := &completionEntry{
		kind:        kind,
		done:        make(chan struct{}),
		settledDone: make(chan struct{}),
	}
	entry.handle = cgo.NewHandle(entry)
	entry.handleKey = uintptr(entry.handle)
	entry.enableCancellation(ctx)
	return entry
}

func newSendCompletionEntry(ctx context.Context, send *sendRetryState) *completionEntry {
	entry := &completionEntry{
		kind:        completionSendRetry,
		send:        send,
		done:        make(chan struct{}),
		settledDone: make(chan struct{}),
	}
	entry.handle = cgo.NewHandle(entry)
	entry.handleKey = uintptr(entry.handle)
	entry.enableCancellation(ctx)
	return entry
}

func (e *completionEntry) enableCancellation(ctx context.Context) {
	if ctx != nil && ctx.Done() != nil {
		stop := context.AfterFunc(ctx, func() {
			e.cancel(ctx.Err())
		})
		e.mu.Lock()
		if e.publicDone || e.settled {
			e.mu.Unlock()
			stop()
			return
		}
		e.stopCancel = stop
		e.mu.Unlock()
	}
}

func (e *completionEntry) deleteHandle() {
	if e == nil {
		return
	}
	e.deleteOnce.Do(func() { e.handle.Delete() })
}

func (e *completionEntry) publish(completionID uint64) {
	e.mu.Lock()
	if !e.settled {
		e.completion = completionID
		e.published = true
		e.settleIfJoinedLocked()
	}
	e.mu.Unlock()
}

func (e *completionEntry) publishSendWait(completionID uint64) {
	e.mu.Lock()
	if !e.settled {
		e.completion = completionID
		e.published = true
	}
	e.mu.Unlock()
}

func (e *completionEntry) finishSend(err error) {
	e.mu.Lock()
	if e.settled {
		e.mu.Unlock()
		return
	}
	e.published = true
	e.settled = true
	if !e.publicDone {
		e.err = err
		e.publicDone = true
		close(e.done)
	}
	close(e.settledDone)
	stop := e.stopCancel
	e.stopCancel = nil
	e.mu.Unlock()
	if stop != nil {
		stop()
	}
}

func (e *completionEntry) setSendWaiting(waiting bool) {
	if e == nil || e.owner == nil {
		return
	}
	e.owner.setSendWaiting(e, waiting)
}

func (e *completionEntry) failSubmit() {
	e.mu.Lock()
	if !e.settled {
		e.published = true
		e.captured = true
		e.settled = true
		close(e.settledDone)
	}
	stop := e.stopCancel
	e.stopCancel = nil
	e.mu.Unlock()
	if stop != nil {
		stop()
	}
}

func (e *completionEntry) cancel(err error) {
	if err == nil {
		return
	}
	if e.kind == completionSendRetry {
		e.cancelSend(err)
		return
	}
	e.mu.Lock()
	if !e.publicDone {
		e.err = err
		e.publicDone = true
		close(e.done)
	}
	e.mu.Unlock()
}

func (e *completionEntry) cancelSend(err error) {
	e.attemptMu.Lock()
	defer e.attemptMu.Unlock()

	e.mu.Lock()
	if e.publicDone {
		e.mu.Unlock()
		return
	}
	e.err = err
	e.publicDone = true
	close(e.done)
	waiting := e.published && !e.settled
	if !waiting {
		e.settled = true
		close(e.settledDone)
	}
	stop := e.stopCancel
	e.stopCancel = nil
	e.mu.Unlock()

	// A canceled caller no longer owns a logical send to retry. If Core already
	// owns a wait token, retain only this entry as a payload-free tombstone until
	// the matching WRITABLE record is pulled.
	if e.send != nil && e.send.payload != nil {
		e.send.payload.close()
	}
	if stop != nil {
		stop()
	}
}

func (e *completionEntry) capture(parts []*Message, err error) {
	e.mu.Lock()
	if e.captured || e.settled {
		e.mu.Unlock()
		MultipartClose(parts)
		return
	}
	e.captured = true
	if e.publicDone {
		e.mu.Unlock()
		MultipartClose(parts)
		e.mu.Lock()
	} else {
		e.parts = parts
		e.err = err
	}
	e.settleIfJoinedLocked()
	e.mu.Unlock()
}

func (e *completionEntry) settleIfJoinedLocked() {
	if !e.published || !e.captured || e.settled {
		return
	}
	e.settled = true
	if !e.publicDone {
		e.publicDone = true
		close(e.done)
	}
	close(e.settledDone)
	stop := e.stopCancel
	e.stopCancel = nil
	if stop != nil {
		go stop()
	}
}

func (e *completionEntry) shutdown() {
	e.attemptMu.Lock()
	defer e.attemptMu.Unlock()

	shutdownErr := error(&SubmitError{Result: SubmitTerminated, nativeErrno: int(C.ESHUTDOWN)})
	if e.kind == completionRequest {
		shutdownErr = &RequestError{Result: RequestTerminated, nativeErrno: int(C.ESHUTDOWN)}
	}

	e.mu.Lock()
	if e.settled {
		e.mu.Unlock()
		return
	}
	parts := e.parts
	e.parts = nil
	e.published = true
	e.captured = true
	if !e.publicDone {
		e.err = shutdownErr
		e.publicDone = true
		close(e.done)
	}
	e.settled = true
	close(e.settledDone)
	stop := e.stopCancel
	e.stopCancel = nil
	e.mu.Unlock()
	if e.send != nil && e.send.payload != nil {
		e.send.payload.close()
	}
	MultipartClose(parts)
	if stop != nil {
		stop()
	}
}

func (e *completionEntry) waitSend() error {
	<-e.done
	e.mu.Lock()
	err := e.err
	e.mu.Unlock()
	return err
}

func (e *completionEntry) waitRequest() ([]*Message, error) {
	<-e.done
	e.mu.Lock()
	parts := e.parts
	e.parts = nil
	err := e.err
	e.mu.Unlock()
	if err != nil {
		MultipartClose(parts)
		return nil, err
	}
	return parts, nil
}

func (e *completionEntry) waitSettled() { <-e.settledDone }

type runtimeCompletionDrain struct {
	poller            unsafe.Pointer
	events            C.short
	observedWaitEpoch uint64
	pollOutSuppressed bool
	stop              chan struct{}
	done              chan struct{}
}

// completionOwner owns the only drain path for one socket. Runtime polling
// and public Poller.Wait transfer this ownership; they never drain together.
type completionOwner struct {
	socket unsafe.Pointer

	mu            sync.Mutex
	entries       map[uintptr]*completionEntry
	publicOwner   *Poller
	runtime       *runtimeCompletionDrain
	sendWaiters   int
	sendWaitEpoch uint64
	shutdown      bool
}

func newCompletionOwner(socket unsafe.Pointer) *completionOwner {
	return &completionOwner{socket: socket, entries: make(map[uintptr]*completionEntry)}
}

func (o *completionOwner) register(entry *completionEntry) error {
	if o == nil || entry == nil {
		return &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.shutdown {
		return &SubmitError{Result: SubmitInvalidState, nativeErrno: int(C.ESHUTDOWN)}
	}
	entry.owner = o
	o.entries[entry.handleKey] = entry
	if o.publicOwner == nil && o.runtime == nil {
		if err := o.startRuntimeLocked(); err != nil {
			delete(o.entries, entry.handleKey)
			return err
		}
	}
	return nil
}

func (o *completionOwner) unregister(entry *completionEntry) {
	if o == nil || entry == nil {
		return
	}
	o.mu.Lock()
	if current := o.entries[entry.handleKey]; current == entry {
		if entry.sendWaiting {
			entry.sendWaiting = false
			o.sendWaiters--
		}
		delete(o.entries, entry.handleKey)
	}
	o.mu.Unlock()
	entry.deleteHandle()
}

func (o *completionOwner) startRuntimeLocked() error {
	poller := C.zlink_poller_new()
	if poller == nil {
		return configErrorFromErrno(currentErrno())
	}
	events := C.short(C.ZLINK_POLLCOMPLETION)
	if o.sendWaiters > 0 {
		events |= C.short(C.ZLINK_POLLOUT)
	}
	if err := configErrorFromResult(C.zlink_poller_add(
		poller, o.socket, nil, events)); err != nil {
		handle := poller
		_ = closeErrorFromResult(C.zlink_poller_destroy(&handle))
		return err
	}
	runtime := &runtimeCompletionDrain{
		poller: poller,
		events: events,
		stop:   make(chan struct{}),
		done:   make(chan struct{}),
	}
	o.runtime = runtime
	go o.runtimeLoop(runtime)
	return nil
}

func (o *completionOwner) setSendWaiting(entry *completionEntry, waiting bool) {
	if o == nil || entry == nil {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if current := o.entries[entry.handleKey]; current != entry || entry.sendWaiting == waiting {
		return
	}
	entry.sendWaiting = waiting
	if waiting {
		o.sendWaiters++
		o.sendWaitEpoch++
	} else {
		o.sendWaiters--
	}
}

func (o *completionOwner) runtimeEvents(runtime *runtimeCompletionDrain) (C.short, uint64, bool) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.shutdown || o.runtime != runtime || o.publicOwner != nil || len(o.entries) == 0 {
		return 0, 0, false
	}
	events := C.short(C.ZLINK_POLLCOMPLETION)
	if o.sendWaiters > 0 && (!runtime.pollOutSuppressed || runtime.observedWaitEpoch != o.sendWaitEpoch) {
		events |= C.short(C.ZLINK_POLLOUT)
	}
	return events, o.sendWaitEpoch, true
}

func (o *completionOwner) runtimeLoop(runtime *runtimeCompletionDrain) {
	defer close(runtime.done)
	for {
		select {
		case <-runtime.stop:
			return
		default:
		}
		// One non-blocking readiness turn at a time keeps progress on the Go
		// scheduler without a dedicated OS thread, sleep, or timer.
		goruntime.Gosched()
		events, waitEpoch, active := o.runtimeEvents(runtime)
		if !active {
			if o.exitInactiveRuntime(runtime) {
				return
			}
			continue
		}
		if events != runtime.events {
			if err := configErrorFromResult(C.zlink_poller_modify(
				runtime.poller, o.socket, events)); err != nil {
				o.failRuntimeLoop(runtime)
				return
			}
			runtime.events = events
		}

		var event C.zlink_poller_event_t
		var result C.zlink_config_result_t
		count := C.zlink_poller_wait(runtime.poller, &event, 1, 0, &result)
		if count > 0 {
			if PollEventFlag(event.events)&PollOut != 0 {
				runtime.observedWaitEpoch = waitEpoch
				runtime.pollOutSuppressed = true
			}
			if _, err := o.drain(false); err != nil {
				o.failRuntimeLoop(runtime)
				return
			}
			continue
		}
		if count < 0 {
			errno := currentErrno()
			if errno != int(C.EINTR) && errno != int(C.EAGAIN) {
				o.failRuntimeLoop(runtime)
				return
			}
		}
	}
}

// exitInactiveRuntime rechecks the condition which made runtimeEvents inactive.
// A submit may register between those two calls; in that case this loop still
// owns the runtime and must keep running.
func (o *completionOwner) exitInactiveRuntime(runtime *runtimeCompletionDrain) bool {
	if o == nil || runtime == nil {
		return true
	}
	o.mu.Lock()
	if o.shutdown || o.runtime != runtime || o.publicOwner != nil {
		o.mu.Unlock()
		return true
	}
	if len(o.entries) != 0 {
		o.mu.Unlock()
		return false
	}
	// Keep the runtime published while its poller is destroyed. Socket close
	// takes the same owner lock before deciding which runtime to join, so it
	// cannot tear down the socket concurrently with this registered poller.
	handle := runtime.poller
	_ = closeErrorFromResult(C.zlink_poller_destroy(&handle))
	runtime.poller = nil
	o.runtime = nil
	o.mu.Unlock()
	return true
}

func (o *completionOwner) failRuntimeLoop(runtime *runtimeCompletionDrain) {
	if o == nil || runtime == nil {
		return
	}
	o.mu.Lock()
	if o.shutdown || o.runtime != runtime || o.publicOwner != nil {
		o.mu.Unlock()
		return
	}
	entries := make([]*completionEntry, 0, len(o.entries))
	for _, entry := range o.entries {
		entries = append(entries, entry)
	}
	// As in idle retirement, keep close from tearing down the socket until the
	// failed runtime poller is no longer registered on it.
	handle := runtime.poller
	_ = closeErrorFromResult(C.zlink_poller_destroy(&handle))
	runtime.poller = nil
	o.runtime = nil
	o.mu.Unlock()
	failCompletionEntries(entries)
}

func failCompletionEntries(entries []*completionEntry) {
	for _, entry := range entries {
		if entry == nil {
			continue
		}
		if entry.kind == completionRequest {
			entry.cancel(&RequestError{Result: RequestInternalError, nativeErrno: int(C.EIO)})
			continue
		}
		entry.cancel(&SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EIO)})
	}
}

func stopRuntimeCompletionDrain(runtime *runtimeCompletionDrain) {
	if runtime == nil {
		return
	}
	close(runtime.stop)
	<-runtime.done
	handle := runtime.poller
	_ = closeErrorFromResult(C.zlink_poller_destroy(&handle))
}

func (o *completionOwner) transferToPublic(poller *Poller) error {
	if o == nil || poller == nil {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	o.mu.Lock()
	if o.shutdown {
		o.mu.Unlock()
		return &ConfigError{Result: ConfigInvalidState, nativeErrno: int(C.ESHUTDOWN)}
	}
	if o.publicOwner != nil && o.publicOwner != poller {
		o.mu.Unlock()
		return &ConfigError{Result: ConfigInvalidState, nativeErrno: int(C.EBUSY)}
	}
	if o.publicOwner == poller {
		o.mu.Unlock()
		return nil
	}
	o.publicOwner = poller
	runtime := o.runtime
	o.runtime = nil
	o.mu.Unlock()
	stopRuntimeCompletionDrain(runtime)
	return nil
}

func (o *completionOwner) transferToRuntime(poller *Poller) {
	if o == nil || poller == nil {
		return
	}
	o.mu.Lock()
	if o.shutdown || o.publicOwner != poller {
		o.mu.Unlock()
		return
	}
	o.publicOwner = nil
	var failed []*completionEntry
	if len(o.entries) > 0 && o.runtime == nil {
		if err := o.startRuntimeLocked(); err != nil {
			failed = make([]*completionEntry, 0, len(o.entries))
			for _, entry := range o.entries {
				failed = append(failed, entry)
			}
		}
	}
	o.mu.Unlock()
	failCompletionEntries(failed)
}

type completionDrainResult struct {
	processed          int
	requestCompletions int
}

func (o *completionOwner) drain(waitForPublish bool) (completionDrainResult, error) {
	var drained completionDrainResult
	for {
		var completion C.zlink_completion_t
		completion.struct_size = C.uint32_t(C.sizeof_zlink_completion_t)
		result := C.zlink_completion_recv(
			o.socket, &completion, C.zlink_recv_flags_t(C.ZLINK_RECV_FLAGS_DONTWAIT))
		if result == C.ZLINK_RECV_NO_DATA {
			return drained, nil
		}
		if err := recvErrorFromResult(result); err != nil {
			return drained, err
		}

		key := uintptr(completion.user_context)
		// user_context is an opaque integer token encoded as void*. Clear that
		// integer-shaped pointer before capture can grow this goroutine's stack.
		completion.user_context = nil
		o.mu.Lock()
		entry := o.entries[key]
		o.mu.Unlock()
		parts, request, terminal, err := captureNativeCompletion(entry, &completion, key)
		if entry != nil && request {
			entry.capture(parts, err)
			if waitForPublish {
				entry.waitSettled()
			}
			o.unregister(entry)
			drained.requestCompletions++
		} else if entry != nil && terminal {
			o.unregister(entry)
		} else {
			MultipartClose(parts)
		}
		drained.processed++
	}
}

func (e *completionEntry) captureWritable(completion *C.zlink_completion_t, contextKey uintptr) bool {
	if e == nil || completion == nil || e.kind != completionSendRetry || e.send == nil {
		return true
	}
	e.attemptMu.Lock()

	completionID := uint64(completion.completion_id)
	peerRID := routingIDFromC(completion.peer_rid)

	e.mu.Lock()
	if e.settled {
		e.mu.Unlock()
		e.attemptMu.Unlock()
		return true
	}
	if !e.published {
		e.mu.Unlock()
		e.setSendWaiting(false)
		e.send.payload.close()
		e.finishSend(&SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)})
		e.attemptMu.Unlock()
		return true
	}
	expectedID := e.completion
	e.mu.Unlock()

	if completionID != expectedID {
		// The expected native token is still live. Fail the caller and release its
		// packet, but retain this entry so the eventual matching record cannot
		// outlive and alias a deleted cgo handle.
		e.mu.Lock()
		if !e.publicDone {
			e.err = &SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)}
			e.publicDone = true
			close(e.done)
		}
		stop := e.stopCancel
		e.stopCancel = nil
		e.mu.Unlock()
		e.send.payload.close()
		if stop != nil {
			stop()
		}
		e.attemptMu.Unlock()
		return false
	}

	if completion.kind != C.ZLINK_COMPLETION_WRITABLE ||
		contextKey != e.handleKey ||
		!e.send.matchesTarget(peerRID) {
		e.setSendWaiting(false)
		e.send.payload.close()
		e.finishSend(&SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)})
		e.attemptMu.Unlock()
		return true
	}

	sendResult := C.zlink_send_complete_result_t(completion.send_result)
	terminalErrno := int(completion.send_terminal_errno)
	if sendResult == C.ZLINK_SEND_TERMINAL {
		if terminalErrno == 0 {
			terminalErrno = int(C.EIO)
		}
		e.setSendWaiting(false)
		e.send.payload.close()
		e.finishSend(&SubmitError{Result: SubmitNotAdmitted, nativeErrno: terminalErrno})
		e.attemptMu.Unlock()
		return true
	}
	if sendResult != C.ZLINK_SEND_ADMITTED || terminalErrno != 0 {
		e.setSendWaiting(false)
		e.send.payload.close()
		e.finishSend(&SubmitError{Result: SubmitInternalError, nativeErrno: int(C.EPROTO)})
		e.attemptMu.Unlock()
		return true
	}

	e.mu.Lock()
	if e.settled || !e.published || e.completion != completionID {
		settled := e.settled
		e.mu.Unlock()
		e.attemptMu.Unlock()
		return settled
	}
	e.published = false
	e.completion = 0
	publicDone := e.publicDone
	e.mu.Unlock()
	e.setSendWaiting(false)
	if publicDone {
		e.send.payload.close()
		e.finishSend(nil)
		e.attemptMu.Unlock()
		return true
	}
	e.attemptMu.Unlock()
	return e.attemptSend()
}

func captureNativeCompletion(
	entry *completionEntry,
	completion *C.zlink_completion_t,
	contextKey uintptr,
) (parts []*Message, request bool, terminal bool, err error) {
	defer C.zlink_completion_close(completion)
	if entry == nil {
		return nil, false, false, nil
	}
	if entry.kind == completionSendRetry {
		return nil, false, entry.captureWritable(completion, contextKey), nil
	}
	if completion.kind != C.ZLINK_COMPLETION_REQUEST {
		return nil, true, true, &RequestError{Result: RequestInternalError, nativeErrno: int(C.EPROTO)}
	}
	result := RequestResult(completion.request_result)
	if result != RequestOK {
		return nil, true, true, requestCompletionError(result)
	}
	parts, err = adoptCompletionParts(completion)
	return parts, true, true, err
}

func adoptCompletionParts(completion *C.zlink_completion_t) ([]*Message, error) {
	count := int(completion.reply_part_count)
	if count == 0 || completion.reply_parts == nil {
		return nil, nil
	}
	raw := unsafe.Slice(completion.reply_parts, count)
	parts := make([]*Message, 0, count)
	for i := range raw {
		message := &Message{}
		if err := configErrorFromResult(C.zlink_msg_init(&message.msg)); err != nil {
			MultipartClose(parts)
			return nil, err
		}
		if err := configErrorFromResult(C.zlink_msg_move(&message.msg, &raw[i])); err != nil {
			_ = message.Close()
			MultipartClose(parts)
			return nil, err
		}
		parts = append(parts, message)
	}
	return parts, nil
}

func (o *completionOwner) shutdownOwner() {
	if o == nil {
		return
	}
	o.mu.Lock()
	if o.shutdown {
		o.mu.Unlock()
		return
	}
	o.shutdown = true
	runtime := o.runtime
	o.runtime = nil
	entries := make([]*completionEntry, 0, len(o.entries))
	for _, entry := range o.entries {
		entries = append(entries, entry)
	}
	o.entries = make(map[uintptr]*completionEntry)
	o.mu.Unlock()

	stopRuntimeCompletionDrain(runtime)
	for _, entry := range entries {
		entry.shutdown()
		entry.deleteHandle()
	}
}
