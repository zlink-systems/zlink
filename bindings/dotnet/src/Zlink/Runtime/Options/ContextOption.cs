// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal enum ContextOption
{
    IoThreads = 1,
    MaxSockets = 2,
    SocketLimit = 3,
    ThreadPriority = 3,
    ThreadSchedPolicy = 4,
    MaxMsgSz = 5,
    MsgTSize = 6,
    ThreadAffinityCpuAdd = 7,
    ThreadAffinityCpuRemove = 8,
    ThreadNamePrefix = 9,
    Blocky = 10,
    AutoHwmEnabled = 12,
    AutoHwmRecalcDebounce = 14,
    AutoHwmProfile = 17,
    AutoHwmMsgUnitBytes = 18
}
