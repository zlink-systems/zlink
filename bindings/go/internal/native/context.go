// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"sync"
	"sync/atomic"
	"unsafe"
)

type Version struct {
	Major int
	Minor int
	Patch int
}

func RuntimeVersion() Version {
	var major C.int
	var minor C.int
	var patch C.int
	C.zlink_version(&major, &minor, &patch)
	return Version{Major: int(major), Minor: int(minor), Patch: int(patch)}
}

type Context struct {
	handle           atomic.Pointer[byte]
	closed           atomic.Bool
	closeMu          sync.Mutex
	socketsMu        sync.Mutex
	sockets          map[*socketCore]struct{}
	optionsMu        sync.RWMutex
	options          *ContextOptions
	threadNamePrefix string
}

type ContextOptions struct {
	ctx *Context
}

// CoreHwmBudgetSnapshot is an immutable value copy of Core's ABI-v1 budget snapshot.
type CoreHwmBudgetSnapshot struct {
	ABIVersion, StructSize                                               uint32
	BudgetGeneration, MeasurementEpoch                                   uint64
	ConfiguredMemoryLimitBytes, ResolvedMemoryLimitBytes                 uint64
	ConfiguredCoreBudgetBytes, EffectiveCoreBudgetBytes                  uint64
	TotalPlannedHwmBytes, TotalAppliedHwmBytes, ManualReservedHwmBytes   uint64
	CoreQueueAccountedBytes, ApplicationAccountedBytes                   uint64
	CurrentAccountedBytes, ProvisionalAccountedBytes, PeakAccountedBytes uint64
	CompletionCurrentAccountedBytes, CompletionPeakAccountedBytes        uint64
	CompletionPendingMessageCount, TotalMessagingAccountedBytes          uint64
	MonitorQueueAppliedHwmBytes, MonitorQueueAccountedBytes              uint64
	TotalInstanceAppliedHwmBytes, TotalInstanceAccountedBytes            uint64
	OversizeAdmissionCount, LargestOversizeMessageBytes                  uint64
	ActiveDirectionalQueueCount, ActiveCompletionDirectionalQueueCount   uint64
	ActiveSendQueueCount, ActiveReceiveQueueCount                        uint64
	OutstandingApplicationLeaseCount, RetiredQueueCount                  uint64
	DeferredOriginCreditBytes, UnlimitedManualQueueCount                 uint64
	BlockedRatioPPM, Flags                                               uint32
	ReservedUint64                                                       [8]uint64
}

func (s CoreHwmBudgetSnapshot) BudgetPlanningActive() bool { return s.Flags&(1<<0) != 0 }
func (s CoreHwmBudgetSnapshot) BudgetInsufficient() bool   { return s.Flags&(1<<1) != 0 }
func (s CoreHwmBudgetSnapshot) AggregateHwmValid() bool    { return s.Flags&(1<<2) != 0 }
func (s CoreHwmBudgetSnapshot) AggregateOverflow() bool    { return s.Flags&(1<<3) != 0 }

type AutoHwmProfile int

const (
	AutoHwmProfileCompact    AutoHwmProfile = AutoHwmProfile(C.ZLINK_AUTO_HWM_PROFILE_COMPACT)
	AutoHwmProfileLowLatency AutoHwmProfile = AutoHwmProfile(C.ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY)
	AutoHwmProfileBalanced   AutoHwmProfile = AutoHwmProfile(C.ZLINK_AUTO_HWM_PROFILE_BALANCED)
	AutoHwmProfileThroughput AutoHwmProfile = AutoHwmProfile(C.ZLINK_AUTO_HWM_PROFILE_THROUGHPUT)
)

type AutoHwmRecalcReason uint32

const (
	AutoHwmRecalcReasonNone           AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_NONE)
	AutoHwmRecalcReasonInitial        AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_INITIAL)
	AutoHwmRecalcReasonRoleChange     AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_ROLE_CHANGE)
	AutoHwmRecalcReasonPolicyToggle   AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_POLICY_TOGGLE)
	AutoHwmRecalcReasonRefresh        AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_REFRESH)
	AutoHwmRecalcReasonDeferredShrink AutoHwmRecalcReason = AutoHwmRecalcReason(C.ZLINK_AUTO_HWM_RECALC_REASON_DEFERRED_SHRINK)
)

func NewContext() (*Context, error) {
	handle, cerr := C.zlink_ctx_new()
	if handle == nil {
		return nil, configErrorFromErrno(cgoErrno(cerr))
	}
	ctx := &Context{sockets: make(map[*socketCore]struct{})}
	ctx.handle.Store((*byte)(handle))
	ctx.options = &ContextOptions{ctx: ctx}
	return ctx, nil
}

func (c *Context) raw() unsafe.Pointer {
	if c == nil {
		return nil
	}
	return unsafe.Pointer(c.handle.Load())
}

func (c *Context) Close() error {
	if c == nil {
		return nil
	}
	c.closeMu.Lock()
	defer c.closeMu.Unlock()
	if c.closed.Load() || c.raw() == nil {
		return nil
	}
	c.shutdownSocketCompletions()
	handle := c.raw()
	rc, cerr := C.zlink_ctx_shutdown(handle)
	if err := closeErrorFromCall(rc, cerr); err != nil {
		return err
	}
	rc, cerr = C.zlink_ctx_term(handle)
	if err := closeErrorFromCall(rc, cerr); err != nil {
		return err
	}
	c.closed.Store(true)
	c.handle.Store(nil)
	return nil
}

func (c *Context) Shutdown() error {
	if c == nil || c.closed.Load() {
		return nil
	}
	c.shutdownSocketCompletions()
	rc, cerr := C.zlink_ctx_shutdown(c.raw())
	return closeErrorFromCall(rc, cerr)
}

func (c *Context) registerSocket(socket *socketCore) {
	if c == nil || socket == nil {
		return
	}
	c.socketsMu.Lock()
	c.sockets[socket] = struct{}{}
	c.socketsMu.Unlock()
}

func (c *Context) unregisterSocket(socket *socketCore) {
	if c == nil || socket == nil {
		return
	}
	c.socketsMu.Lock()
	delete(c.sockets, socket)
	c.socketsMu.Unlock()
}

func (c *Context) shutdownSocketCompletions() {
	if c == nil {
		return
	}
	c.socketsMu.Lock()
	sockets := make([]*socketCore, 0, len(c.sockets))
	for socket := range c.sockets {
		sockets = append(sockets, socket)
	}
	c.socketsMu.Unlock()
	for _, socket := range sockets {
		socket.completion.shutdownOwner()
	}
}

// RecalculateAutoHwm forces an automatic HWM recalculation. Returns *ConfigError on failure.
func (c *Context) RecalculateAutoHwm() error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	nativeResult0, nativeErr0 := C.zlink_ctx_auto_hwm_recalculate(c.raw())
	return configErrorFromCall(nativeResult0, nativeErr0)
}

func (c *Context) CoreHwmBudgetSnapshot() (CoreHwmBudgetSnapshot, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return CoreHwmBudgetSnapshot{}, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.zlink_auto_hwm_budget_snapshot_t
	raw.abi_version = C.ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1
	raw.struct_size = C.uint32_t(C.sizeof_zlink_auto_hwm_budget_snapshot_t)
	nativeResult1, nativeErr1 := C.zlink_ctx_get_auto_hwm_budget_snapshot(c.raw(), &raw)
	if err := configErrorFromCall(nativeResult1, nativeErr1); err != nil {
		return CoreHwmBudgetSnapshot{}, err
	}
	var reserved [8]uint64
	for index := range reserved {
		reserved[index] = uint64(raw.reserved_u64[index])
	}
	return CoreHwmBudgetSnapshot{
		uint32(raw.abi_version), uint32(raw.struct_size), uint64(raw.budget_generation), uint64(raw.measurement_epoch),
		uint64(raw.configured_memory_limit_bytes), uint64(raw.resolved_memory_limit_bytes),
		uint64(raw.configured_core_budget_bytes), uint64(raw.effective_core_budget_bytes), uint64(raw.total_planned_hwm_bytes),
		uint64(raw.total_applied_hwm_bytes), uint64(raw.manual_reserved_hwm_bytes), uint64(raw.core_queue_accounted_bytes),
		uint64(raw.application_accounted_bytes), uint64(raw.current_accounted_bytes), uint64(raw.provisional_accounted_bytes),
		uint64(raw.peak_accounted_bytes), uint64(raw.completion_current_accounted_bytes), uint64(raw.completion_peak_accounted_bytes),
		uint64(raw.completion_pending_message_count), uint64(raw.total_messaging_accounted_bytes), uint64(raw.monitor_queue_applied_hwm_bytes),
		uint64(raw.monitor_queue_accounted_bytes), uint64(raw.total_instance_applied_hwm_bytes), uint64(raw.total_instance_accounted_bytes),
		uint64(raw.oversize_admission_count), uint64(raw.largest_oversize_message_bytes), uint64(raw.active_directional_queue_count),
		uint64(raw.active_completion_directional_queue_count), uint64(raw.active_send_queue_count), uint64(raw.active_receive_queue_count),
		uint64(raw.outstanding_application_lease_count), uint64(raw.retired_queue_count), uint64(raw.deferred_origin_credit_bytes),
		uint64(raw.unlimited_manual_queue_count), uint32(raw.blocked_ratio_ppm), uint32(raw.flags), reserved,
	}, nil
}

func (c *Context) ResetCoreHwmBudgetMetrics() error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	nativeResult2, nativeErr2 := C.zlink_ctx_reset_auto_hwm_budget_metrics(c.raw())
	return configErrorFromCall(nativeResult2, nativeErr2)
}

func (c *Context) Options() *ContextOptions {
	if c == nil {
		return nil
	}
	return c.options
}

func (c *Context) PairSocket() (*PairSocket, error) {
	return newPairSocket(c)
}

func (c *Context) PubSocket() (*PubSocket, error) {
	return newPubSocket(c, C.ZLINK_SOCKET_PUB)
}

func (c *Context) SubSocket() (*SubSocket, error) {
	return newSubSocket(c, C.ZLINK_SOCKET_SUB)
}

func (c *Context) DealerSocket() (*DealerSocket, error) {
	return newDealerSocket(c)
}

func (c *Context) RouterSocket() (*RouterSocket, error) {
	return newRouterSocket(c)
}

func (c *Context) XPubSocket() (*XPubSocket, error) {
	return newXPubSocket(c)
}

func (c *Context) XSubSocket() (*XSubSocket, error) {
	return newXSubSocket(c)
}

func (c *Context) StreamSocket() (*StreamSocket, error) {
	return newStreamSocket(c)
}
