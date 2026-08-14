using System.Runtime.InteropServices;

namespace Systems.Zlink.Runtime.Native;

[StructLayout(LayoutKind.Sequential)]
internal struct ZlinkSocketMonitorOpenOptions
{
    public uint Events;
    public ulong MonitorHwmBytes;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ZlinkMonitorStatus
{
    internal const uint CurrentAbiVersion = 3;

    public uint AbiVersion;
    public uint StructSize;
    public int MonitorSourceKind;
    public uint StateFlags;
    public uint DetailFlags;
    public ulong SndPendingMsgs;
    public ulong RcvPendingMsgs;
    public ulong SndPendingBytes;
    public ulong RcvPendingBytes;
    public uint AutoHwmEnabled;
    public uint AutoHwmProfile;
    public uint AutoHwmRole;
    public uint AutoHwmPolicyClass;
    public ulong AutoHwmPlannedSndHwmBytes;
    public ulong AutoHwmPlannedRcvHwmBytes;
    public ulong AutoHwmAppliedSndHwmBytes;
    public ulong AutoHwmAppliedRcvHwmBytes;
    public int AutoHwmEffectiveSndbuf;
    public int AutoHwmEffectiveRcvbuf;
    public ulong AutoHwmLastRecalcMs;
    public uint AutoHwmLastRecalcReason;
    public uint AutoHwmSendBlockedRatioPpm;
    public ulong AutoHwmDeferredSndHwmBytes;
    public ulong AutoHwmDeferredRcvHwmBytes;
    public uint AutoHwmDeferredSndHwmValid;
    public uint AutoHwmDeferredRcvHwmValid;
    public ulong SndBytesInFlight;
    public ulong RcvBytesInFlight;
    public ulong MinimumCoreMessageChargeBytes;
    public ulong OversizeMessageAdmissionCount;
    public ulong OversizeMessageAdmissionMaxBytes;
}
