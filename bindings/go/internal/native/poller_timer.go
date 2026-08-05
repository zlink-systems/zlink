// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"

extern void goZlinkTimerTrampoline(void *timer_, uint64_t fire_count_, uintptr_t userdata_);

static inline int zlink_timer_handler_go_local(void *timer, uintptr_t userdata) {
	return zlink_timer_handler(timer, (zlink_timer_handler_fn)goZlinkTimerTrampoline, (void *)userdata);
}

static inline void *zlink_userdata_from_handle(uintptr_t handle) {
	return (void *)handle;
}

static inline uintptr_t zlink_handle_from_userdata(void *userdata) {
	return (uintptr_t)userdata;
}
*/
import "C"

import (
	"runtime/cgo"
	"sync"
	"time"
	"unsafe"
)

// PollEventFlag is a bitmask of poll readiness event flags.
type PollEventFlag int16

const (
	PollIn         PollEventFlag = 1
	PollOut        PollEventFlag = 2
	PollErr        PollEventFlag = 4
	PollPri        PollEventFlag = 8
	PollCompletion PollEventFlag = 32
)

// PollSourceKind identifies the kind of source in a PollEvent.
type PollSourceKind int32

const (
	PollSourceSocket PollSourceKind = 1
	PollSourceFD     PollSourceKind = 2
	PollSourceTimer  PollSourceKind = 3
)

type PollItem struct {
	Socket  SocketTarget
	Fd      int
	Events  PollEventFlag
	REvents PollEventFlag
}

type PollEvent struct {
	SourceKind PollSourceKind
	socket     uintptr
	Fd         int
	timer      uintptr
	Slot       uintptr
	Revents    PollEventFlag
}

type pollerEntryKind int

const (
	pollerEntrySocket pollerEntryKind = iota + 1
	pollerEntryFD
	pollerEntryTimer
)

type pollerEntry struct {
	kind   pollerEntryKind
	socket SocketTarget
	raw    unsafe.Pointer
	fd     int
	timer  *Timer
	slot   uintptr
	events PollEventFlag
	owner  *socketCore
}

type Poller struct {
	handle     unsafe.Pointer
	mu         sync.Mutex
	waitMu     sync.Mutex
	waitEvents []C.zlink_poller_event_t
	sockets    map[uintptr]*pollerEntry
	fds        map[int]*pollerEntry
	timers     map[uintptr]*pollerEntry
	closed     bool
}

type timerCallbackState struct {
	dispatcher *callbackDispatcher
	timer      *Timer
	handler    func(*Timer, uint64)
}

func newTimerCallbackState(timer *Timer, handler func(*Timer, uint64)) *timerCallbackState {
	return &timerCallbackState{
		dispatcher: newCallbackDispatcher(),
		timer:      timer,
		handler:    handler,
	}
}

func (s *timerCallbackState) close() {
	if s == nil {
		return
	}
	s.dispatcher.close()
}

type Timer struct {
	handle   unsafe.Pointer
	closed   bool
	callback cgo.Handle
}

func NewTimer() (*Timer, error) {
	handle := C.zlink_timer_new()
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
	}
	return &Timer{handle: handle}, nil
}

func (t *Timer) raw() unsafe.Pointer {
	if t == nil {
		return nil
	}
	return t.handle
}

func (t *Timer) Start(intervalNs, repeatCount uint64) error {
	if t == nil || t.closed || t.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return configErrorFromResult(C.zlink_timer_start(t.handle, C.uint64_t(intervalNs), C.uint64_t(repeatCount)))
}

func (t *Timer) Stop() error {
	if t == nil || t.closed || t.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return configErrorFromResult(C.zlink_timer_stop(t.handle))
}

// Recv drains the next timer fire. Returns (count, true, nil) when data is
// available, (0, false, nil) when no timer fire is pending (EAGAIN), or
// (0, false, err) on error. Value-return form is allowed for monitor/timer
// control-plane APIs by doc/spec/bindings/go/README.md §Receive And Subscribe Shape.
func (t *Timer) Recv() (uint64, bool, error) {
	if t == nil || t.closed || t.handle == nil {
		return 0, false, &RecvError{Result: RecvTerminated, nativeErrno: int(C.EFAULT)}
	}
	var fireCount C.uint64_t
	rc := C.zlink_timer_recv(t.handle, &fireCount)
	if rc == C.zlink_recv_result_t(RecvNoData) {
		return 0, false, nil
	}
	if err := recvErrorFromResult(rc); err != nil {
		return 0, false, err
	}
	return uint64(fireCount), true, nil
}

func (t *Timer) OnFire(handler func(timer *Timer, fireCount uint64)) error {
	if handler == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if t == nil || t.closed || t.handle == nil {
		return &HandlerError{Result: HandlerInvalidArgument, nativeErrno: int(C.EFAULT)}
	}
	state := newTimerCallbackState(t, handler)
	handle := cgo.NewHandle(state)
	if err := handlerErrorFromResult(C.zlink_timer_handler_go_local(t.handle, C.uintptr_t(handle))); err != nil {
		state.close()
		handle.Delete()
		return err
	}
	if t.callback != 0 {
		releaseCallbackHandle(t.callback)
	}
	t.callback = handle
	return nil
}

func (t *Timer) Close() error {
	if t == nil || t.closed || t.handle == nil {
		return nil
	}
	handle := t.handle
	if err := closeErrorFromResult(C.zlink_timer_destroy(&handle)); err != nil {
		return err
	}
	if t.callback != 0 {
		releaseCallbackHandle(t.callback)
		t.callback = 0
	}
	t.handle = nil
	t.closed = true
	return nil
}

func NewPoller() (*Poller, error) {
	handle := C.zlink_poller_new()
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
	}
	return &Poller{
		handle:  handle,
		sockets: make(map[uintptr]*pollerEntry),
		fds:     make(map[int]*pollerEntry),
		timers:  make(map[uintptr]*pollerEntry),
	}, nil
}

func (p *Poller) raw() unsafe.Pointer {
	if p == nil {
		return nil
	}
	return p.handle
}

func (p *Poller) Size() int {
	if p == nil {
		return 0
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return 0
	}
	var err C.zlink_config_result_t
	size := C.zlink_poller_size(p.handle, &err)
	if err != 0 {
		return 0
	}
	return int(size)
}

func (p *Poller) AddSocket(socket SocketTarget, events PollEventFlag, slot uintptr) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	raw, err := socketHandle(socket)
	if err != nil {
		return err
	}
	entry := p.makeEntry(pollerEntrySocket, socket, raw, 0, nil, slot, events)
	if err := configErrorFromResult(C.zlink_poller_add(p.handle, raw, entry.userDataPtr(), C.short(events))); err != nil {
		return err
	}
	if events&PollCompletion != 0 {
		acquireExternalRequestProgress(raw, entry.owner)
	}
	p.sockets[uintptr(raw)] = entry
	return nil
}

func (p *Poller) ModifySocket(socket SocketTarget, events PollEventFlag) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	// Completion ownership is acquired at registration time and released at
	// removal time. Core rejects adding the flag through modify, so reject it
	// before touching the Go registry as well.
	if events&PollCompletion != 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	raw, err := socketHandle(socket)
	if err != nil {
		return err
	}
	entry := p.sockets[uintptr(raw)]
	if entry != nil && entry.events&PollCompletion != 0 {
		// A completion registration owns native completion processing until the
		// registration is removed. Change that mode with remove + add.
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if err := configErrorFromResult(C.zlink_poller_modify(p.handle, raw, C.short(events))); err != nil {
		return err
	}
	if entry != nil {
		entry.events = events
	}
	return nil
}

func (p *Poller) RemoveSocket(socket SocketTarget) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	raw, err := socketHandle(socket)
	if err != nil {
		return err
	}
	if err := configErrorFromResult(C.zlink_poller_remove(p.handle, raw)); err != nil {
		return err
	}
	entry := p.sockets[uintptr(raw)]
	if entry != nil && entry.events&PollCompletion != 0 {
		releaseExternalRequestProgress(raw, entry.owner)
	}
	delete(p.sockets, uintptr(raw))
	return nil
}

func (p *Poller) AddFd(fd int, events PollEventFlag, slot uintptr) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	entry := p.makeEntry(pollerEntryFD, nil, nil, fd, nil, slot, events)
	if err := configErrorFromResult(C.zlink_poller_add_fd(p.handle, C.zlink_fd_t(fd), entry.userDataPtr(), C.short(events))); err != nil {
		return err
	}
	p.fds[fd] = entry
	return nil
}

func (p *Poller) ModifyFd(fd int, events PollEventFlag) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return configErrorFromResult(C.zlink_poller_modify_fd(p.handle, C.zlink_fd_t(fd), C.short(events)))
}

func (p *Poller) RemoveFd(fd int) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if err := configErrorFromResult(C.zlink_poller_remove_fd(p.handle, C.zlink_fd_t(fd))); err != nil {
		return err
	}
	delete(p.fds, fd)
	return nil
}

func (p *Poller) AddTimer(timer *Timer, slot uintptr) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if timer == nil || timer.closed || timer.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	entry := p.makeEntry(pollerEntryTimer, nil, nil, 0, timer, slot, PollIn)
	if err := configErrorFromResult(C.zlink_poller_add_timer(p.handle, timer.handle, entry.userDataPtr())); err != nil {
		return err
	}
	p.timers[uintptr(timer.handle)] = entry
	return nil
}

func (p *Poller) RemoveTimer(timer *Timer) error {
	if p == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if timer == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if timer.handle == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if err := configErrorFromResult(C.zlink_poller_remove_timer(p.handle, timer.handle)); err != nil {
		return err
	}
	delete(p.timers, uintptr(timer.handle))
	return nil
}

func (p *Poller) Wait(events []PollEvent, timeout time.Duration) (int, error) {
	if p == nil {
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	p.waitMu.Lock()
	defer p.waitMu.Unlock()
	p.mu.Lock()
	if p.closed || p.handle == nil {
		p.mu.Unlock()
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	handle := p.handle
	p.mu.Unlock()
	if len(events) == 0 {
		return 0, configErrorFromResult(C.ZLINK_CONFIG_INVALID_ARGUMENT)
	}
	var errCode C.zlink_config_result_t
	ms, err := durationToMillis(timeout)
	if err != nil {
		return 0, err
	}
	if cap(p.waitEvents) < len(events) {
		p.waitEvents = make([]C.zlink_poller_event_t, len(events))
	}
	nativeEvents := p.waitEvents[:len(events)]
	count := C.zlink_poller_wait(
		handle,
		(*C.zlink_poller_event_t)(unsafe.Pointer(&nativeEvents[0])),
		C.int(len(events)),
		C.long(ms),
		&errCode)
	if count < 0 {
		if errCode != 0 {
			if errCode == C.ZLINK_CONFIG_INTERNAL_ERROR && currentErrno() == int(C.EINTR) {
				return 0, nil
			}
			return 0, configErrorFromResult(errCode)
		}
		if currentErrno() == int(C.EINTR) {
			return 0, nil
		}
		return 0, configErrorFromErrno(currentErrno())
	}
	for i := 0; i < int(count); i++ {
		event := nativeEvents[i]
		events[i] = PollEvent{
			SourceKind: PollSourceKind(event.source_kind),
			Fd:         int(event.fd),
			Slot:       uintptr(event.user_data),
			Revents:    PollEventFlag(event.events),
		}
	}
	return int(count), nil
}

func (p *Poller) Close() error {
	if p == nil {
		return nil
	}
	p.waitMu.Lock()
	defer p.waitMu.Unlock()
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed || p.handle == nil {
		return nil
	}
	handle := p.handle
	if err := closeErrorFromResult(C.zlink_poller_destroy(&handle)); err != nil {
		return err
	}
	for k, entry := range p.sockets {
		if entry != nil && entry.events&PollCompletion != 0 {
			releaseExternalRequestProgress(entry.raw, entry.owner)
		}
		delete(p.sockets, k)
	}
	for k := range p.fds {
		delete(p.fds, k)
	}
	for k := range p.timers {
		delete(p.timers, k)
	}
	p.handle = nil
	p.closed = true
	return nil
}

func (p *Poller) makeEntry(kind pollerEntryKind, socket SocketTarget, raw unsafe.Pointer, fd int, timer *Timer, slot uintptr, events PollEventFlag) *pollerEntry {
	var owner *socketCore
	if target, ok := socket.(interface{ completionOwner() *socketCore }); ok {
		owner = target.completionOwner()
	}
	return &pollerEntry{kind: kind, socket: socket, raw: raw, fd: fd, timer: timer, slot: slot, events: events, owner: owner}
}

func (e *pollerEntry) userDataPtr() unsafe.Pointer {
	if e == nil {
		return nil
	}
	return C.zlink_userdata_from_handle(C.uintptr_t(e.slot))
}

func Poll(items []PollItem, timeout time.Duration) (int, error) {
	var errCode C.zlink_config_result_t
	ms, err := durationToMillis(timeout)
	if err != nil {
		return 0, err
	}
	var rawItems *C.zlink_pollitem_t
	var converted []C.zlink_pollitem_t
	if len(items) > 0 {
		converted = make([]C.zlink_pollitem_t, len(items))
		for i, item := range items {
			if item.Socket != nil {
				converted[i].socket = item.Socket.raw()
			}
			converted[i].fd = C.zlink_fd_t(item.Fd)
			converted[i].events = C.short(item.Events)
		}
		rawItems = &converted[0]
	}
	count := C.zlink_poll(rawItems, C.int(len(converted)), C.long(ms), &errCode)
	if count < 0 {
		if errCode != 0 {
			return 0, configErrorFromResult(errCode)
		}
		return 0, configErrorFromErrno(currentErrno())
	}
	for i := range items {
		items[i].REvents = PollEventFlag(converted[i].revents)
	}
	return int(count), nil
}
