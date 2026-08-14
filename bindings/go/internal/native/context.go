// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"math"
	"runtime/debug"
	"sync"
	"sync/atomic"
	"time"
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
	optionsMu        sync.RWMutex
	options          *ContextOptions
	threadNamePrefix string
}

type ContextOptions struct {
	ctx *Context
}

// CoreHwmBudgetSnapshot is an immutable value copy of Core's ABI-v1 budget snapshot.
type CoreHwmBudgetSnapshot struct {
	ABIVersion, StructSize                                                        uint32
	BudgetGeneration, MeasurementEpoch                                            uint64
	ConfiguredMemoryLimitBytes, RuntimeMemoryLimitBytes, ResolvedMemoryLimitBytes uint64
	ConfiguredCoreBudgetBytes, EffectiveCoreBudgetBytes                           uint64
	TotalPlannedHwmBytes, TotalAppliedHwmBytes, ManualReservedHwmBytes            uint64
	CoreQueueAccountedBytes, ApplicationAccountedBytes                            uint64
	CurrentAccountedBytes, ProvisionalAccountedBytes, PeakAccountedBytes          uint64
	CompletionCurrentAccountedBytes, CompletionPeakAccountedBytes                 uint64
	CompletionPendingMessageCount, TotalMessagingAccountedBytes                   uint64
	MonitorQueueAppliedHwmBytes, MonitorQueueAccountedBytes                       uint64
	TotalInstanceAppliedHwmBytes, TotalInstanceAccountedBytes                     uint64
	OversizeAdmissionCount, LargestOversizeMessageBytes                           uint64
	ActiveDirectionalQueueCount, ActiveCompletionDirectionalQueueCount            uint64
	ActiveSendQueueCount, ActiveReceiveQueueCount                                 uint64
	OutstandingApplicationLeaseCount, RetiredQueueCount                           uint64
	DeferredOriginCreditBytes, UnlimitedManualQueueCount                          uint64
	BlockedRatioPPM, Flags                                                        uint32
	ReservedUint64                                                                [8]uint64
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
	handle := C.zlink_ctx_new()
	if handle == nil {
		return nil, configErrorFromErrno(currentErrno())
	}
	ctx := &Context{}
	ctx.handle.Store((*byte)(handle))
	ctx.options = &ContextOptions{ctx: ctx}
	if limit := debug.SetMemoryLimit(-1); limit > 0 && limit < math.MaxInt64 {
		if err := ctx.setUint64DataOption(C.ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES, uint64(limit)); err != nil {
			_ = ctx.Close()
			return nil, err
		}
	}
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
	handle := c.raw()
	if err := closeErrorFromResult(C.zlink_ctx_term(handle)); err != nil {
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
	return closeErrorFromResult(C.zlink_ctx_shutdown(c.raw()))
}

// RecalculateAutoHwm forces an automatic HWM recalculation. Returns *ConfigError on failure.
func (c *Context) RecalculateAutoHwm() error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return configErrorFromResult(C.zlink_ctx_auto_hwm_recalculate(c.raw()))
}

func (c *Context) CoreHwmBudgetSnapshot() (CoreHwmBudgetSnapshot, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return CoreHwmBudgetSnapshot{}, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.zlink_auto_hwm_budget_snapshot_t
	raw.abi_version = C.ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1
	raw.struct_size = C.uint32_t(C.sizeof_zlink_auto_hwm_budget_snapshot_t)
	if err := configErrorFromResult(C.zlink_ctx_get_auto_hwm_budget_snapshot(c.raw(), &raw)); err != nil {
		return CoreHwmBudgetSnapshot{}, err
	}
	var reserved [8]uint64
	for index := range reserved {
		reserved[index] = uint64(raw.reserved_u64[index])
	}
	return CoreHwmBudgetSnapshot{
		uint32(raw.abi_version), uint32(raw.struct_size), uint64(raw.budget_generation), uint64(raw.measurement_epoch),
		uint64(raw.configured_memory_limit_bytes), uint64(raw.runtime_memory_limit_bytes), uint64(raw.resolved_memory_limit_bytes),
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
	return configErrorFromResult(C.zlink_ctx_reset_auto_hwm_budget_metrics(c.raw()))
}

func (c *Context) Options() *ContextOptions {
	if c == nil {
		return nil
	}
	return c.options
}

func (o *ContextOptions) SetIOThreads(value int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_IO_THREADS, value)
}

func (o *ContextOptions) IOThreads() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_IO_THREADS)
}

func (o *ContextOptions) SetMaxSockets(value int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_MAX_SOCKETS, value)
}

func (o *ContextOptions) MaxSockets() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_MAX_SOCKETS)
}

func (o *ContextOptions) SocketLimit() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_SOCKET_LIMIT)
}

func (o *ContextOptions) SetThreadPriority(value int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_THREAD_PRIORITY, value)
}

func (o *ContextOptions) ThreadPriority() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_THREAD_PRIORITY)
}

func (o *ContextOptions) SetThreadSchedulingPolicy(value int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_THREAD_SCHED_POLICY, value)
}

func (o *ContextOptions) ThreadSchedulingPolicy() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_THREAD_SCHED_POLICY)
}

func (o *ContextOptions) SetThreadNamePrefix(value string) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	if len(value) > 16 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	var ptr unsafe.Pointer
	var n C.size_t
	if len(value) > 0 {
		cstr := C.CString(value)
		defer C.free(unsafe.Pointer(cstr))
		ptr = unsafe.Pointer(cstr)
		n = C.size_t(len(value))
	}
	if err := configErrorFromResult(C.zlink_ctx_set_data(ctx.raw(), C.ZLINK_THREAD_NAME_PREFIX, ptr, n)); err != nil {
		return err
	}
	ctx.optionsMu.Lock()
	ctx.threadNamePrefix = value
	ctx.optionsMu.Unlock()
	return nil
}

func (o *ContextOptions) ThreadNamePrefix() (string, error) {
	ctx, err := o.context()
	if err != nil {
		return "", err
	}
	ctx.optionsMu.RLock()
	defer ctx.optionsMu.RUnlock()
	return ctx.threadNamePrefix, nil
}

func (o *ContextOptions) SetAutoHwmEnabled(value bool) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	raw := 0
	if value {
		raw = 1
	}
	return ctx.setIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_ENABLE, raw)
}

func (o *ContextOptions) AutoHwmEnabled() (bool, error) {
	ctx, err := o.context()
	if err != nil {
		return false, err
	}
	value, err := ctx.getIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_ENABLE)
	return value != 0, err
}

func (o *ContextOptions) SetAutoHwmRecalcDebounce(value time.Duration) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	ms := value / time.Millisecond
	if ms > math.MaxInt32 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	return ctx.setIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS, int(ms))
}

func (o *ContextOptions) AutoHwmRecalcDebounce() (time.Duration, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	value, err := ctx.getIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS)
	return time.Duration(value) * time.Millisecond, err
}

func (o *ContextOptions) SetMaxMessageSize(value int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_MAX_MSGSZ, value)
}

func (o *ContextOptions) MaxMessageSize() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_MAX_MSGSZ)
}

func (o *ContextOptions) MessageStructSize() (int, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getIntOption(C.ZLINK_MSG_T_SIZE)
}

func (o *ContextOptions) AddThreadAffinity(cpu int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_THREAD_AFFINITY_CPU_ADD, cpu)
}

func (o *ContextOptions) RemoveThreadAffinity(cpu int) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_THREAD_AFFINITY_CPU_REMOVE, cpu)
}

func (o *ContextOptions) SetBlocky(value bool) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	raw := 0
	if value {
		raw = 1
	}
	return ctx.setIntOption(C.ZLINK_CTX_OPT_BLOCKY, raw)
}

func (o *ContextOptions) Blocky() (bool, error) {
	ctx, err := o.context()
	if err != nil {
		return false, err
	}
	value, err := ctx.getIntOption(C.ZLINK_CTX_OPT_BLOCKY)
	return value != 0, err
}

func (o *ContextOptions) SetCoreHwmProfile(value AutoHwmProfile) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_PROFILE, int(value))
}

func (o *ContextOptions) CoreHwmProfile() (AutoHwmProfile, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	value, err := ctx.getIntOption(C.ZLINK_CTX_OPT_AUTO_HWM_PROFILE)
	return AutoHwmProfile(value), err
}

func (o *ContextOptions) SetCoreHwmMemoryLimitBytes(value uint64) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setUint64DataOption(C.ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES, value)
}

func (o *ContextOptions) CoreHwmMemoryLimitBytes() (uint64, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getUint64DataOption(C.ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES)
}

func (o *ContextOptions) SetCoreHwmBudgetBytes(value uint64) error {
	ctx, err := o.context()
	if err != nil {
		return err
	}
	return ctx.setUint64DataOption(C.ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES, value)
}

func (o *ContextOptions) CoreHwmBudgetBytes() (uint64, error) {
	ctx, err := o.context()
	if err != nil {
		return 0, err
	}
	return ctx.getUint64DataOption(C.ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES)
}

func (o *ContextOptions) context() (*Context, error) {
	if o == nil || o.ctx == nil || o.ctx.closed.Load() || o.ctx.raw() == nil {
		return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	return o.ctx, nil
}

func (c *Context) setIntOption(option C.zlink_ctx_option_t, value int) error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	if value < math.MinInt32 || value > math.MaxInt32 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	return configErrorFromResult(ConfigResult(C.zlink_ctx_set(c.raw(), option, C.int(value))))
}

func (c *Context) getIntOption(option C.zlink_ctx_option_t) (int, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var result C.zlink_config_result_t
	value := C.zlink_ctx_get(c.raw(), option, &result)
	if result != 0 {
		return 0, configErrorFromResult(result)
	}
	return int(value), nil
}

func (c *Context) setUint64DataOption(option C.zlink_ctx_option_t, value uint64) error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	raw := C.uint64_t(value)
	return configErrorFromResult(C.zlink_ctx_set_data(
		c.raw(),
		option,
		unsafe.Pointer(&raw),
		C.size_t(unsafe.Sizeof(raw)),
	))
}

func (c *Context) getUint64DataOption(option C.zlink_ctx_option_t) (uint64, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.uint64_t
	size := C.size_t(unsafe.Sizeof(raw))
	err := configErrorFromResult(C.zlink_ctx_get_data(
		c.raw(),
		option,
		unsafe.Pointer(&raw),
		&size,
	))
	return uint64(raw), err
}

func durationToMillis(value time.Duration) (int32, error) {
	if value < 0 {
		return -1, nil
	}
	ms := value / time.Millisecond
	if ms > math.MaxInt32 {
		return 0, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	return int32(ms), nil
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
