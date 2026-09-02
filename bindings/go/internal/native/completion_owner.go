// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"

static inline void *zlink_go_completion_context(uintptr_t value) {
	return (void *)value;
}

static inline uintptr_t zlink_go_completion_context_value(void *value) {
	return (uintptr_t)value;
}
*/
import "C"

import (
	"context"
	"runtime/cgo"
	"sync"
	"unsafe"
)

type completionOperationKind uint8

const (
	completionSend completionOperationKind = iota + 1
	completionRequest
)

// completionEntry is the two-phase join between the native submit return and
// a completion captured by whichever drain owner currently owns the socket.
// The cgo handle is only an opaque lookup key; Core never dereferences it.
type completionEntry struct {
	kind completionOperationKind

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
	if ctx != nil && ctx.Done() != nil {
		entry.stopCancel = context.AfterFunc(ctx, func() {
			entry.cancel(ctx.Err())
		})
	}
	return entry
}

func (e *completionEntry) userContext() unsafe.Pointer {
	return C.zlink_go_completion_context(C.uintptr_t(e.handleKey))
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
	e.mu.Lock()
	if !e.publicDone {
		e.err = err
		e.publicDone = true
		close(e.done)
	}
	e.mu.Unlock()
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
	shutdownErr := error(&SubmitError{Result: SubmitTerminated, nativeErrno: int(C.ESHUTDOWN)})
	if e.kind == completionRequest {
		shutdownErr = &RequestError{Result: RequestTerminated, nativeErrno: int(C.ESHUTDOWN)}
	}

	e.mu.Lock()
	parts := e.parts
	e.parts = nil
	e.err = shutdownErr
	e.published = true
	e.captured = true
	if !e.publicDone {
		e.publicDone = true
		close(e.done)
	}
	if !e.settled {
		e.settled = true
		close(e.settledDone)
	}
	stop := e.stopCancel
	e.stopCancel = nil
	e.mu.Unlock()
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
	poller unsafe.Pointer
	stop   chan struct{}
	done   chan struct{}
}

// completionOwner owns the only drain path for one socket. Runtime polling
// and public Poller.Wait transfer this ownership; they never drain together.
type completionOwner struct {
	socket unsafe.Pointer

	mu          sync.Mutex
	entries     map[uintptr]*completionEntry
	publicOwner *Poller
	runtime     *runtimeCompletionDrain
	shutdown    bool
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
	if err := configErrorFromResult(C.zlink_poller_add(
		poller, o.socket, nil, C.short(C.ZLINK_POLLCOMPLETION))); err != nil {
		handle := poller
		_ = closeErrorFromResult(C.zlink_poller_destroy(&handle))
		return err
	}
	runtime := &runtimeCompletionDrain{
		poller: poller,
		stop:   make(chan struct{}),
		done:   make(chan struct{}),
	}
	o.runtime = runtime
	go o.runtimeLoop(runtime)
	return nil
}

func (o *completionOwner) runtimeLoop(runtime *runtimeCompletionDrain) {
	defer close(runtime.done)
	for {
		select {
		case <-runtime.stop:
			return
		default:
		}

		var event C.zlink_poller_event_t
		var result C.zlink_config_result_t
		count := C.zlink_poller_wait(runtime.poller, &event, 1, 25, &result)
		if count > 0 {
			if _, err := o.drain(false); err != nil {
				return
			}
			continue
		}
		if count < 0 {
			errno := currentErrno()
			if errno != int(C.EINTR) && errno != int(C.EAGAIN) {
				return
			}
		}
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
	defer o.mu.Unlock()
	if o.shutdown || o.publicOwner != poller {
		return
	}
	o.publicOwner = nil
	if len(o.entries) > 0 && o.runtime == nil {
		_ = o.startRuntimeLocked()
	}
}

func (o *completionOwner) drain(waitForPublish bool) (int, error) {
	processed := 0
	for {
		var completion C.zlink_completion_t
		completion.struct_size = C.uint32_t(C.sizeof_zlink_completion_t)
		result := C.zlink_completion_recv(
			o.socket, &completion, C.zlink_recv_flags_t(C.ZLINK_RECV_FLAGS_DONTWAIT))
		if result == C.ZLINK_RECV_NO_DATA {
			return processed, nil
		}
		if err := recvErrorFromResult(result); err != nil {
			return processed, err
		}

		key := uintptr(C.zlink_go_completion_context_value(completion.user_context))
		o.mu.Lock()
		entry := o.entries[key]
		o.mu.Unlock()
		parts, err := captureNativeCompletion(entry, &completion)
		if entry != nil {
			entry.capture(parts, err)
			if waitForPublish {
				entry.waitSettled()
			}
			o.unregister(entry)
		} else {
			MultipartClose(parts)
		}
		processed++
	}
}

func captureNativeCompletion(entry *completionEntry, completion *C.zlink_completion_t) ([]*Message, error) {
	defer C.zlink_completion_close(completion)
	if entry == nil {
		return nil, nil
	}
	if entry.kind == completionSend {
		if completion.kind != C.ZLINK_COMPLETION_SEND || completion.send_result != C.ZLINK_SEND_ADMITTED {
			errno := int(completion.send_terminal_errno)
			if errno == 0 {
				errno = int(C.EIO)
			}
			return nil, &SubmitError{Result: SubmitNotAdmitted, nativeErrno: errno}
		}
		return nil, nil
	}
	if completion.kind != C.ZLINK_COMPLETION_REQUEST {
		return nil, &RequestError{Result: RequestInternalError, nativeErrno: int(C.EPROTO)}
	}
	result := RequestResult(completion.request_result)
	if result != RequestOK {
		return nil, requestCompletionError(result)
	}
	return adoptCompletionParts(completion)
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
