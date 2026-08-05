// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;
using System.Runtime.InteropServices;

namespace Systems.Zlink;

internal static class MonitorConverters
{
    internal static MonitorEvent FromNative(ref ZlinkMonitorEvent evt)
    {
        string local;
        string remote;
        unsafe
        {
            fixed (byte* localPtr = evt.LocalAddr)
            fixed (byte* remotePtr = evt.RemoteAddr)
            {
                local = NativeHelpers.ReadString(localPtr, 256);
                remote = NativeHelpers.ReadString(remotePtr, 256);
            }
        }

        return new MonitorEvent((MonitorEventType)(evt.Event & 0xFFFFFFFFuL),
            (uint)evt.Value, RoutingIdCodec.ToRoutingId(
                NativeHelpers.ReadRoutingId(ref evt.RoutingId)),
            local, remote);
    }

    internal static MonitorStatus FromNative(ref ZlinkMonitorStatus native)
    {
        var expectedSize = (uint)Marshal.SizeOf<ZlinkMonitorStatus>();
        if (native.AbiVersion != ZlinkMonitorStatus.CurrentAbiVersion
            || native.StructSize != expectedSize)
            throw new NotSupportedException(
                $"Unsupported monitor status ABI {native.AbiVersion} "
                + $"with structure size {native.StructSize}.");
        return new MonitorStatus(in native);
    }
}
