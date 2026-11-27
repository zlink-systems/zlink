// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import (
	"math"
	"time"
	"unsafe"
)

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
	nativeResult3, nativeErr3 := C.zlink_ctx_set_data(ctx.raw(), C.ZLINK_THREAD_NAME_PREFIX, ptr, n)
	if err := configErrorFromCall(nativeResult3, nativeErr3); err != nil {
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
	rc, cerr := C.zlink_ctx_set(c.raw(), option, C.int(value))
	return configErrorFromCall(rc, cerr)
}

func (c *Context) getIntOption(option C.zlink_ctx_option_t) (int, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var result C.zlink_config_result_t
	value, cerr := C.zlink_ctx_get(c.raw(), option, &result)
	if result != 0 {
		return 0, configErrorFromCall(result, cerr)
	}
	return int(value), nil
}

func (c *Context) setUint64DataOption(option C.zlink_ctx_option_t, value uint64) error {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	raw := C.uint64_t(value)
	nativeResult4, nativeErr4 := C.zlink_ctx_set_data(
		c.raw(),
		option,
		unsafe.Pointer(&raw),
		C.size_t(unsafe.Sizeof(raw)),
	)
	return configErrorFromCall(nativeResult4, nativeErr4)
}

func (c *Context) getUint64DataOption(option C.zlink_ctx_option_t) (uint64, error) {
	if c == nil || c.closed.Load() || c.raw() == nil {
		return 0, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var raw C.uint64_t
	size := C.size_t(unsafe.Sizeof(raw))
	nativeResult5, nativeErr5 := C.zlink_ctx_get_data(
		c.raw(),
		option,
		unsafe.Pointer(&raw),
		&size,
	)
	err := configErrorFromCall(nativeResult5, nativeErr5)
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
