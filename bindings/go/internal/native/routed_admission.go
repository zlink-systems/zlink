// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <errno.h>
#include <stdint.h>
#include "zlink.h"

extern void goZlinkRoutedSendReadyTrampoline(void *subject_, zlink_routed_send_ready_event_t *event_, uintptr_t userdata_);

static inline int zlink_routed_send_ready_handler_go_local(void *socket_, uintptr_t userdata_) {
    return zlink_routed_send_ready_handler(
        socket_,
        (zlink_routed_send_ready_handler_fn)goZlinkRoutedSendReadyTrampoline,
        (void *)userdata_);
}
*/
import "C"

import (
	"context"
	"errors"
	"runtime"
	"runtime/cgo"
	"sync"
	"time"
	"unsafe"
)

type routedAdmissionRole uint8

const routedCloseBusyRetryLimit = 1024

const (
	routedDealer routedAdmissionRole = iota
	routedRouter
)

type routedTargetKey struct {
	routingID               RoutingID
	transportPairID         uint64
	transportPairGeneration uint64
}

type routedPendingState uint8

const (
	routedUnarmed routedPendingState = iota
	routedQueued
	routedReady
	routedWaiting
	routedInFlight
)

type routedPendingOperation struct {
	id                  uint64
	target              C.zlink_routed_submit_target_t
	key                 routedTargetKey
	state               routedPendingState
	observedWake        uint64
	forcedError         error
	deadline            time.Time
	deadlineError       error
	attemptBeforeExpiry bool
	context             context.Context
	attempt             func(unsafe.Pointer, *C.zlink_routed_submit_target_t) error
	accepted            func()
	terminal            func(error)
}

// routedAdmission is the single owner of exact-target waiting for one native
// DEALER or ROUTER. The worker attempts only ready targets; a target that is
// waiting for HWM credit occupies neither the caller nor the record gate.
type routedAdmission struct {
	core *socketCore
	role routedAdmissionRole

	attemptGate sync.Mutex

	mu              sync.Mutex
	changed         *sync.Cond
	closing         bool
	closed          bool
	nextID          uint64
	operations      map[uint64]*routedPendingOperation
	pendingByTarget map[routedTargetKey][]*routedPendingOperation
	readyTargets    []routedTargetKey
	readyHead       int
	readySet        map[routedTargetKey]struct{}
	wakeVersions    map[routedTargetKey]uint64
	done            chan struct{}
}

func newRoutedAdmission(core *socketCore, role routedAdmissionRole) (*routedAdmission, error) {
	admission := &routedAdmission{
		core:            core,
		role:            role,
		operations:      make(map[uint64]*routedPendingOperation),
		pendingByTarget: make(map[routedTargetKey][]*routedPendingOperation),
		readySet:        make(map[routedTargetKey]struct{}),
		wakeVersions:    make(map[routedTargetKey]uint64),
		done:            make(chan struct{}),
	}
	admission.changed = sync.NewCond(&admission.mu)
	go admission.run()

	handle := cgo.NewHandle(admission)
	err := core.replaceCallback(handle, &core.routedReadyHandle, nil, func() error {
		return handlerErrorFromResult(C.zlink_routed_send_ready_handler_go_local(
			core.raw(), C.uintptr_t(handle)))
	})
	if err != nil {
		admission.shutdown(routedTerminalError(int(C.ECANCELED)))
		handle.Delete()
		return nil, err
	}
	core.routedAdmission = admission
	return admission, nil
}

func (a *routedAdmission) enqueue(routerRID *RoutingID, operation *routedPendingOperation) (bool, error) {
	if a == nil || operation == nil {
		return false, &SubmitError{Result: SubmitInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	target, err := a.selectTarget(routerRID)
	if err != nil {
		return false, err
	}
	operation.target = target
	operation.key = routedTargetKeyFromTarget(target)

	a.mu.Lock()
	if a.closed || a.closing {
		a.mu.Unlock()
		return false, routedTerminalError(int(C.ETERM))
	}
	a.nextID++
	operation.id = a.nextID
	a.operations[operation.id] = operation
	queue := a.pendingByTarget[operation.key]
	effectiveAttemptBeforeExpiry := operation.attemptBeforeExpiry
	if len(queue) != 0 && effectiveAttemptBeforeExpiry {
		operation.attemptBeforeExpiry = false
		effectiveAttemptBeforeExpiry = false
	}
	operation.state = routedUnarmed
	a.pendingByTarget[operation.key] = append(queue, operation)
	a.mu.Unlock()
	return effectiveAttemptBeforeExpiry, nil
}

func (a *routedAdmission) activate(id uint64) bool {
	if a == nil || id == 0 {
		return false
	}
	a.mu.Lock()
	operation := a.operations[id]
	if operation == nil || operation.state != routedUnarmed {
		a.mu.Unlock()
		return false
	}
	queue := a.pendingByTarget[operation.key]
	if len(queue) != 0 && queue[0] == operation {
		operation.state = routedReady
		a.markReadyLocked(operation.key)
	} else {
		operation.state = routedQueued
	}
	a.changed.Signal()
	a.mu.Unlock()
	return true
}

func (a *routedAdmission) selectTarget(routerRID *RoutingID) (C.zlink_routed_submit_target_t, error) {
	var target C.zlink_routed_submit_target_t
	var rid C.zlink_routing_id_t
	var ridPointer *C.zlink_routing_id_t
	if routerRID != nil {
		if routerRID.Size() == 0 {
			return target, &SubmitError{Result: SubmitInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		rid = routerRID.toC()
		ridPointer = &rid
	}

	a.attemptGate.Lock()
	defer a.attemptGate.Unlock()
	a.mu.Lock()
	unavailable := a.closed || a.closing
	a.mu.Unlock()
	if unavailable || a.core == nil || a.core.isClosed() {
		return target, routedTerminalError(int(C.ETERM))
	}
	result := C.zlink_select_routed_submit_target(a.core.raw(), ridPointer, &target)
	if err := submitErrorFromResult(result); err != nil {
		return target, err
	}
	return target, nil
}

func (a *routedAdmission) cancel(id uint64, terminalError error) bool {
	if a == nil || id == 0 || terminalError == nil {
		return false
	}
	var terminal func(error)
	a.mu.Lock()
	operation := a.operations[id]
	if operation == nil {
		a.mu.Unlock()
		return false
	}
	if operation.state == routedInFlight {
		if operation.forcedError == nil {
			operation.forcedError = terminalError
		}
		a.mu.Unlock()
		return true
	}
	a.removeLocked(operation)
	terminal = operation.terminal
	a.changed.Signal()
	a.mu.Unlock()
	if terminal != nil {
		terminal(terminalError)
	}
	return true
}

func (a *routedAdmission) run() {
	defer close(a.done)
	for {
		operation := a.nextReady()
		if operation == nil {
			return
		}

		forcedError, expired, present := a.beforeAttempt(operation)
		if !present {
			continue
		}
		if forcedError != nil {
			a.finishAttempt(operation, forcedError)
			continue
		}
		if expired {
			a.finishAttempt(operation, operation.deadlineError)
			continue
		}

		a.attemptAndFinish(operation)
	}
}

func (a *routedAdmission) beforeAttempt(operation *routedPendingOperation) (error, bool, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.operations[operation.id] != operation {
		return nil, false, false
	}
	expired := !operation.deadline.IsZero() &&
		!operation.attemptBeforeExpiry &&
		!time.Now().Before(operation.deadline)
	return operation.forcedError, expired, true
}

func (a *routedAdmission) nextReady() *routedPendingOperation {
	a.mu.Lock()
	defer a.mu.Unlock()
	for {
		for a.readyHead < len(a.readyTargets) {
			key := a.readyTargets[a.readyHead]
			a.readyHead++
			delete(a.readySet, key)
			queue := a.pendingByTarget[key]
			if len(queue) == 0 || queue[0].state != routedReady {
				continue
			}
			operation := queue[0]
			operation.state = routedInFlight
			operation.observedWake = a.wakeVersions[key]
			if a.readyHead == len(a.readyTargets) {
				a.readyTargets = a.readyTargets[:0]
				a.readyHead = 0
			}
			return operation
		}
		if a.readyHead > 0 {
			a.readyTargets = a.readyTargets[:0]
			a.readyHead = 0
		}
		if a.closed {
			return nil
		}
		a.changed.Wait()
	}
}

func (a *routedAdmission) attemptAndFinish(operation *routedPendingOperation) {
	a.attemptGate.Lock()
	a.mu.Lock()
	unavailable := a.closed || a.closing
	forcedError := operation.forcedError
	if forcedError == nil && operation.context != nil {
		forcedError = operation.context.Err()
	}
	expired := !operation.deadline.IsZero() &&
		!operation.attemptBeforeExpiry &&
		!time.Now().Before(operation.deadline)
	a.mu.Unlock()
	var attemptError error
	if unavailable || a.core == nil || a.core.isClosed() {
		attemptError = routedTerminalError(int(C.ETERM))
	} else if forcedError != nil {
		attemptError = forcedError
	} else if expired {
		attemptError = operation.deadlineError
	} else {
		attemptError = operation.attempt(a.core.raw(), &operation.target)
	}
	accepted, terminal, terminalError := a.finishAttemptState(operation, attemptError)
	a.attemptGate.Unlock()
	runRoutedAttemptCallbacks(accepted, terminal, terminalError)
}

func (a *routedAdmission) finishAttempt(operation *routedPendingOperation, attemptError error) {
	accepted, terminal, terminalError := a.finishAttemptState(operation, attemptError)
	runRoutedAttemptCallbacks(accepted, terminal, terminalError)
}

func (a *routedAdmission) finishAttemptState(operation *routedPendingOperation, attemptError error) (func(), func(error), error) {
	var accepted func()
	var terminal func(error)
	var terminalError error

	a.mu.Lock()
	current := a.operations[operation.id]
	if current == nil {
		a.mu.Unlock()
		return nil, nil, nil
	}
	operation.attemptBeforeExpiry = false
	if operation.forcedError != nil {
		terminalError = operation.forcedError
		a.removeLocked(operation)
		if attemptError == nil {
			accepted = operation.accepted
		}
		terminal = operation.terminal
	} else if attemptError == nil {
		a.removeLocked(operation)
		accepted = operation.accepted
	} else if isSubmitBackpressure(attemptError) {
		if !operation.deadline.IsZero() && !time.Now().Before(operation.deadline) {
			terminalError = operation.deadlineError
			a.removeLocked(operation)
			terminal = operation.terminal
		} else if a.wakeVersions[operation.key] > operation.observedWake {
			operation.state = routedReady
			a.markReadyLocked(operation.key)
		} else {
			operation.state = routedWaiting
		}
	} else {
		terminalError = attemptError
		a.removeLocked(operation)
		terminal = operation.terminal
	}
	a.changed.Signal()
	a.mu.Unlock()
	return accepted, terminal, terminalError
}

func runRoutedAttemptCallbacks(accepted func(), terminal func(error), terminalError error) {
	if accepted != nil {
		accepted()
	}
	if terminal != nil {
		terminal(terminalError)
	}
}

func (a *routedAdmission) onReadyEvent(event C.zlink_routed_send_ready_event_t) {
	key := routedTargetKey{
		routingID:               routingIDFromC(event.peer_rid),
		transportPairID:         uint64(event.transport_pair_id),
		transportPairGeneration: uint64(event.transport_pair_generation),
	}
	a.mu.Lock()
	if a.closed {
		a.mu.Unlock()
		return
	}
	queue := a.pendingByTarget[key]
	if len(queue) == 0 {
		a.mu.Unlock()
		return
	}
	if event.state == C.ZLINK_ROUTED_SEND_TERMINAL {
		terminalError := routedTerminalError(int(event.terminal_errno))
		for _, operation := range queue {
			if operation.forcedError == nil {
				operation.forcedError = terminalError
			}
		}
		if queue[0].state != routedInFlight {
			queue[0].state = routedReady
			a.markReadyLocked(key)
		}
	} else {
		a.wakeVersions[key]++
		if queue[0].state == routedWaiting {
			queue[0].state = routedReady
			a.markReadyLocked(key)
		}
	}
	a.changed.Signal()
	a.mu.Unlock()
}

func (a *routedAdmission) markReadyLocked(key routedTargetKey) {
	if _, exists := a.readySet[key]; exists {
		return
	}
	a.readySet[key] = struct{}{}
	a.readyTargets = append(a.readyTargets, key)
}

func (a *routedAdmission) removeLocked(operation *routedPendingOperation) {
	delete(a.operations, operation.id)
	queue := a.pendingByTarget[operation.key]
	index := -1
	for i, candidate := range queue {
		if candidate.id == operation.id {
			index = i
			break
		}
	}
	if index < 0 {
		return
	}
	queue = append(queue[:index], queue[index+1:]...)
	if len(queue) == 0 {
		delete(a.pendingByTarget, operation.key)
		delete(a.wakeVersions, operation.key)
		return
	}
	a.pendingByTarget[operation.key] = queue
	if index == 0 && queue[0].state != routedUnarmed {
		queue[0].state = routedReady
		a.markReadyLocked(operation.key)
	}
}

func (a *routedAdmission) closeNative() error {
	if a == nil {
		return nil
	}
	a.mu.Lock()
	if a.closed {
		a.mu.Unlock()
		return nil
	}
	a.closing = true
	a.mu.Unlock()

	a.attemptGate.Lock()
	var err error
	for attempt := 0; ; attempt++ {
		err = closeErrorFromResult(C.zlink_close(a.core.raw()))
		if !isCloseBusy(err) || attempt >= routedCloseBusyRetryLimit {
			break
		}
		// A routed-ready callback can wake the worker and let its completion
		// reach the caller just before Core leaves that callback's lifecycle
		// scope. Give only that short EBUSY handoff a bounded chance to finish;
		// a persistent concurrent API still returns CloseBusy.
		runtime.Gosched()
	}
	a.attemptGate.Unlock()
	if err != nil {
		a.mu.Lock()
		a.closing = false
		a.changed.Signal()
		a.mu.Unlock()
		return err
	}
	a.shutdown(routedTerminalError(int(C.ETERM)))
	return nil
}

func (a *routedAdmission) withAttemptGate(attempt func() error) error {
	if a == nil {
		return attempt()
	}
	a.attemptGate.Lock()
	defer a.attemptGate.Unlock()
	a.mu.Lock()
	unavailable := a.closed || a.closing
	a.mu.Unlock()
	if unavailable {
		return routedTerminalError(int(C.ETERM))
	}
	return attempt()
}

func (a *routedAdmission) shutdown(terminalError error) {
	if a == nil {
		return
	}
	var terminals []func(error)
	a.mu.Lock()
	if !a.closed {
		a.closed = true
		a.closing = false
		for _, operation := range a.operations {
			if operation.terminal != nil {
				terminals = append(terminals, operation.terminal)
			}
		}
		a.operations = make(map[uint64]*routedPendingOperation)
		a.pendingByTarget = make(map[routedTargetKey][]*routedPendingOperation)
		a.readyTargets = nil
		a.readySet = make(map[routedTargetKey]struct{})
		a.wakeVersions = make(map[routedTargetKey]uint64)
		a.changed.Broadcast()
	}
	a.mu.Unlock()
	for _, terminal := range terminals {
		terminal(terminalError)
	}
	<-a.done
}

func (a *routedAdmission) close() {
	a.shutdown(routedTerminalError(int(C.ETERM)))
}

func routedTargetKeyFromTarget(target C.zlink_routed_submit_target_t) routedTargetKey {
	return routedTargetKey{
		routingID:               routingIDFromC(target.peer_rid),
		transportPairID:         uint64(target.transport_pair_id),
		transportPairGeneration: uint64(target.transport_pair_generation),
	}
}

func isSubmitBackpressure(err error) bool {
	var submitError *SubmitError
	return errors.As(err, &submitError) && submitError.Result == SubmitBackpressured
}

func isCloseBusy(err error) bool {
	var closeError *CloseError
	return errors.As(err, &closeError) && closeError.Result == CloseBusy
}

func routedTerminalError(nativeErrno int) error {
	if nativeErrno == 0 {
		nativeErrno = int(C.ENOTCONN)
	}
	result := SubmitNotConnected
	switch nativeErrno {
	case int(C.ETERM), int(C.ESHUTDOWN), int(C.ECANCELED):
		result = SubmitTerminated
	case int(C.ENOENT), int(C.EHOSTUNREACH):
		result = SubmitNotFound
	}
	return &SubmitError{Result: result, nativeErrno: nativeErrno}
}

//export goZlinkRoutedSendReadyTrampoline
func goZlinkRoutedSendReadyTrampoline(_ unsafe.Pointer, event *C.zlink_routed_send_ready_event_t, userdata C.uintptr_t) {
	if event == nil {
		return
	}
	admission, ok := safeHandleAs[*routedAdmission](userdata)
	if !ok {
		return
	}
	admission.onReadyEvent(*event)
}
