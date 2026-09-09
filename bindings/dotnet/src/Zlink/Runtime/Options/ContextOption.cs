// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum ContextOption
{
    IoThreads = 1,
    MaxSockets = 2,
    SocketLimit = 3,
    ThreadPriority = 22,
    ThreadSchedPolicy = 4,
    MsgTSize = 6,
    ThreadAffinityCpuAdd = 7,
    ThreadAffinityCpuRemove = 8,
    ThreadNamePrefix = 9,
    Blocky = 10,
    AutoHwmEnabled = 12,
    AutoHwmRecalcDebounce = 14,
    AutoHwmProfile = 17,
    AutoHwmMemoryLimitBytes = 19,
    AutoHwmRuntimeMemoryLimitBytes = 20,
    AutoHwmCoreBudgetBytes = 21
}
