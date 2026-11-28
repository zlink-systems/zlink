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
	"zlink.systems/zlink/contracts"
)

type Version = contracts.Version

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

type CoreHwmBudgetSnapshot = contracts.CoreHwmBudgetSnapshot
type AutoHwmProfile = contracts.AutoHwmProfile
type AutoHwmRecalcReason = contracts.AutoHwmRecalcReason

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
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
	}
	nativeResult0, nativeErr0 := C.zlink_ctx_auto_hwm_recalculate(c.raw())
	return configErrorFromCall(nativeResult0, nativeErr0)
}

func (c *Context) CoreHwmBudgetSnapshot() (CoreHwmBudgetSnapshot, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return CoreHwmBudgetSnapshot{}, newConfigError(ConfigInvalidHandle, int(C.EFAULT))
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
		ABIVersion:                            uint32(raw.abi_version),
		StructSize:                            uint32(raw.struct_size),
		BudgetGeneration:                      uint64(raw.budget_generation),
		MeasurementEpoch:                      uint64(raw.measurement_epoch),
		ConfiguredMemoryLimitBytes:            uint64(raw.configured_memory_limit_bytes),
		ResolvedMemoryLimitBytes:              uint64(raw.resolved_memory_limit_bytes),
		ConfiguredCoreBudgetBytes:             uint64(raw.configured_core_budget_bytes),
		EffectiveCoreBudgetBytes:              uint64(raw.effective_core_budget_bytes),
		TotalPlannedHwmBytes:                  uint64(raw.total_planned_hwm_bytes),
		TotalAppliedHwmBytes:                  uint64(raw.total_applied_hwm_bytes),
		ManualReservedHwmBytes:                uint64(raw.manual_reserved_hwm_bytes),
		CoreQueueAccountedBytes:               uint64(raw.core_queue_accounted_bytes),
		ApplicationAccountedBytes:             uint64(raw.application_accounted_bytes),
		CurrentAccountedBytes:                 uint64(raw.current_accounted_bytes),
		ProvisionalAccountedBytes:             uint64(raw.provisional_accounted_bytes),
		PeakAccountedBytes:                    uint64(raw.peak_accounted_bytes),
		CompletionCurrentAccountedBytes:       uint64(raw.completion_current_accounted_bytes),
		CompletionPeakAccountedBytes:          uint64(raw.completion_peak_accounted_bytes),
		CompletionPendingMessageCount:         uint64(raw.completion_pending_message_count),
		TotalMessagingAccountedBytes:          uint64(raw.total_messaging_accounted_bytes),
		MonitorQueueAppliedHwmBytes:           uint64(raw.monitor_queue_applied_hwm_bytes),
		MonitorQueueAccountedBytes:            uint64(raw.monitor_queue_accounted_bytes),
		TotalInstanceAppliedHwmBytes:          uint64(raw.total_instance_applied_hwm_bytes),
		TotalInstanceAccountedBytes:           uint64(raw.total_instance_accounted_bytes),
		OversizeAdmissionCount:                uint64(raw.oversize_admission_count),
		LargestOversizeMessageBytes:           uint64(raw.largest_oversize_message_bytes),
		ActiveDirectionalQueueCount:           uint64(raw.active_directional_queue_count),
		ActiveCompletionDirectionalQueueCount: uint64(raw.active_completion_directional_queue_count),
		ActiveSendQueueCount:                  uint64(raw.active_send_queue_count),
		ActiveReceiveQueueCount:               uint64(raw.active_receive_queue_count),
		OutstandingApplicationLeaseCount:      uint64(raw.outstanding_application_lease_count),
		RetiredQueueCount:                     uint64(raw.retired_queue_count),
		DeferredOriginCreditBytes:             uint64(raw.deferred_origin_credit_bytes),
		UnlimitedManualQueueCount:             uint64(raw.unlimited_manual_queue_count),
		BlockedRatioPPM:                       uint32(raw.blocked_ratio_ppm),
		Flags:                                 uint32(raw.flags),
		ReservedUint64:                        reserved,
	}, nil
}

func (c *Context) ResetCoreHwmBudgetMetrics() error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return newConfigError(ConfigInvalidHandle, int(C.EFAULT))
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
