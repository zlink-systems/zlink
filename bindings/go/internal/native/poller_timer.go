// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"

typedef struct {
    int count;
    zlink_config_result_t result;
    int native_errno;
} zlink_go_poller_wait_result;

static inline zlink_go_poller_wait_result zlink_go_poller_wait(
    void *poller, zlink_poller_event_t *events, int capacity, long timeout) {
    zlink_go_poller_wait_result out = {0, ZLINK_CONFIG_OK, 0};
    out.count = zlink_poller_wait(poller, events, capacity, timeout, &out.result);
    if (out.count < 0)
        out.native_errno = zlink_errno();
    return out;
}

static inline zlink_config_result_t zlink_go_poller_add_slot(
    void *poller_, void *socket_, uintptr_t slot_, short events_) {
    return zlink_poller_add(poller_, socket_, (void *)slot_, events_);
}

static inline zlink_config_result_t zlink_go_poller_add_fd_slot(
    void *poller_, zlink_fd_t fd_, uintptr_t slot_, short events_) {
    return zlink_poller_add_fd(poller_, fd_, (void *)slot_, events_);
}

static inline zlink_config_result_t zlink_go_poller_add_timer_slot(
    void *poller_, void *timer_, uintptr_t slot_) {
    return zlink_poller_add_timer(poller_, timer_, (void *)slot_);
}
*/
import "C"

import (
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
	kind           pollerEntryKind
	socket         SocketTarget
	raw            unsafe.Pointer
	fd             int
	timer          *Timer
	slot           uintptr
	events         PollEventFlag
	owner          *completionOwner
	ownsCompletion bool
}

type completionOwnerTarget interface {
	completionDrainOwner() *completionOwner
}

func completionOwnerOf(socket SocketTarget) *completionOwner {
	target, ok := socket.(completionOwnerTarget)
	if !ok {
		return nil
	}
	return target.completionDrainOwner()
}

type Poller struct {
	handle     unsafe.Pointer
	mu         sync.Mutex
	waitMu     sync.Mutex
	waitEvents []C.zlink_poller_event_t
	waitSlots  []uintptr
	sockets    map[uintptr]*pollerEntry
	fds        map[int]*pollerEntry
	timers     map[uintptr]*pollerEntry
	closed     bool
}

type Timer struct {
	handle unsafe.Pointer
	closed bool
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

func (t *Timer) Close() error {
	if t == nil || t.closed || t.handle == nil {
		return nil
	}
	handle := t.handle
	if err := closeErrorFromResult(C.zlink_timer_destroy(&handle)); err != nil {
		return err
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

// AddMonitor registers a borrowed socket monitor with a caller slot. Only PollIn
// is supported; other readiness flags return ConfigInvalidArgument. After Wait,
// drain monitor.Recv(RecvFlagsDontWait) until RecvError.Result is RecvNoData.
// Remove the monitor before closing it. This is an alias of AddSocket.
func (p *Poller) AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error {
	return p.AddSocket(monitor, events, slot)
}

// ModifyMonitor changes a monitor's PollIn interest; it is an alias of ModifySocket.
func (p *Poller) ModifyMonitor(monitor *SocketMonitor, events PollEventFlag) error {
	return p.ModifySocket(monitor, events)
}

// RemoveMonitor unregisters a borrowed monitor; it is an alias of RemoveSocket.
func (p *Poller) RemoveMonitor(monitor *SocketMonitor) error {
	return p.RemoveSocket(monitor)
}

// AddSocket registers a socket or *SocketMonitor; monitors support only PollIn.
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
	if _, monitor := socket.(*SocketMonitor); monitor && events & ^PollIn != 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	entry := p.makeEntry(pollerEntrySocket, socket, raw, 0, nil, slot, events)
	entry.owner = completionOwnerOf(socket)
	entry.ownsCompletion = events&PollCompletion != 0
	if entry.ownsCompletion {
		if entry.owner == nil {
			return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		if err := entry.owner.transferToPublic(p); err != nil {
			return err
		}
	}
	if err := configErrorFromResult(C.zlink_go_poller_add_slot(p.handle, raw, C.uintptr_t(entry.slot), C.short(events))); err != nil {
		if entry.ownsCompletion {
			entry.owner.transferToRuntime(p)
		}
		return err
	}
	p.sockets[uintptr(raw)] = entry
	return nil
}

// ModifySocket changes the interest of a socket or monitor; monitors support only PollIn.
func (p *Poller) ModifySocket(socket SocketTarget, events PollEventFlag) error {
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
	if _, monitor := socket.(*SocketMonitor); monitor && events & ^PollIn != 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	entry := p.sockets[uintptr(raw)]
	if entry == nil {
		return &ConfigError{Result: ConfigNotFound, nativeErrno: int(C.ENOENT)}
	}
	hadCompletion := entry.ownsCompletion
	wantsCompletion := events&PollCompletion != 0
	if !hadCompletion && wantsCompletion {
		if entry.owner == nil {
			return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		if err := entry.owner.transferToPublic(p); err != nil {
			return err
		}
	}
	if err := configErrorFromResult(C.zlink_poller_modify(p.handle, raw, C.short(events))); err != nil {
		if !hadCompletion && wantsCompletion {
			entry.owner.transferToRuntime(p)
		}
		return err
	}
	entry.events = events
	entry.ownsCompletion = wantsCompletion
	if hadCompletion && !wantsCompletion {
		entry.owner.transferToRuntime(p)
	}
	return nil
}

// RemoveSocket unregisters a socket or monitor.
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
	entry := p.sockets[uintptr(raw)]
	if err := configErrorFromResult(C.zlink_poller_remove(p.handle, raw)); err != nil {
		return err
	}
	delete(p.sockets, uintptr(raw))
	if entry != nil && entry.ownsCompletion {
		entry.owner.transferToRuntime(p)
	}
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
	if err := configErrorFromResult(C.zlink_go_poller_add_fd_slot(p.handle, C.zlink_fd_t(fd), C.uintptr_t(entry.slot), C.short(events))); err != nil {
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
	if err := configErrorFromResult(C.zlink_go_poller_add_timer_slot(p.handle, timer.handle, C.uintptr_t(entry.slot))); err != nil {
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
	ms, err := durationToMillis(timeout)
	if err != nil {
		return 0, err
	}
	if cap(p.waitEvents) < len(events) {
		p.waitEvents = make([]C.zlink_poller_event_t, len(events))
	}
	nativeEvents := p.waitEvents[:len(events)]
	count, errCode, nativeErrno := nativePollerWait(
		handle, &nativeEvents[0], len(events), int64(ms))
	if count < 0 {
		if (errCode == ConfigOK || errCode == ConfigInternalError) && nativeErrno == int(C.EINTR) {
			return 0, nil
		}
		if errCode != ConfigOK {
			if nativeErrno == 0 {
				nativeErrno = int(C.EIO)
			}
			return 0, &ConfigError{Result: errCode, nativeErrno: nativeErrno}
		}
		return 0, configErrorFromErrno(nativeErrno)
	}
	readyCount := int(count)
	if cap(p.waitSlots) < readyCount {
		p.waitSlots = make([]uintptr, readyCount)
	}
	slots := p.waitSlots[:readyCount]
	for i := 0; i < readyCount; i++ {
		// Slots are opaque integers encoded in native user_data. Remove the
		// integer-shaped pointer from Go-scanned storage before a completion drain
		// can grow this goroutine's stack.
		slots[i] = uintptr(nativeEvents[i].user_data)
		nativeEvents[i].user_data = nil
	}
	out := 0
	for i := 0; i < readyCount; i++ {
		event := nativeEvents[i]
		revents := PollEventFlag(event.events)
		if revents&(PollOut|PollCompletion) != 0 {
			p.mu.Lock()
			entry := p.sockets[uintptr(event.socket)]
			p.mu.Unlock()
			if entry != nil && entry.ownsCompletion && entry.owner != nil {
				drained, drainErr := entry.owner.drain(true)
				if drainErr != nil {
					return 0, drainErr
				}
				// A WRITABLE record advances a managed SEND retry but is not a
				// successful-SEND completion. Keep PollCompletion caller-visible
				// only when this drain also delivered a REQUEST completion.
				if drained.requestCompletions == 0 {
					revents &^= PollCompletion
				} else {
					revents |= PollCompletion
				}
			}
		}
		if revents == 0 {
			continue
		}
		events[out] = PollEvent{
			SourceKind: PollSourceKind(event.source_kind),
			Fd:         int(event.fd),
			Slot:       slots[i],
			Revents:    revents,
		}
		out++
	}
	return out, nil
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
	for _, entry := range p.sockets {
		if entry.ownsCompletion && entry.owner != nil {
			entry.owner.transferToRuntime(p)
		}
	}
	for k := range p.sockets {
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
	return &pollerEntry{kind: kind, socket: socket, raw: raw, fd: fd, timer: timer, slot: slot, events: events}
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

// nativePollerWait captures the thread-local error before cgo can resume Go on
// another OS thread. Both public and runtime polling use this boundary.
func nativePollerWait(poller unsafe.Pointer, events *C.zlink_poller_event_t, capacity int, timeout int64) (int, ConfigResult, int) {
	result := C.zlink_go_poller_wait(poller, events, C.int(capacity), C.long(timeout))
	return int(result.count), ConfigResult(result.result), int(result.native_errno)
}
