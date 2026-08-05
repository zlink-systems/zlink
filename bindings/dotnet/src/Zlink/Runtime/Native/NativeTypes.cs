using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Systems.Zlink.Runtime.Native;

[StructLayout(LayoutKind.Explicit, Size = 64)]
internal unsafe struct ZlinkMsg
{
    // zlink_msg_t is a 64-byte opaque value aligned to sizeof(void *).
    // The pointer-sized field forces native-compatible alignment.
    [FieldOffset(0)] public fixed byte Data[64];

    [FieldOffset(0)] private readonly nuint _align;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ZlinkRoutingId
{
    public byte Size;
    public fixed byte Data[255];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ZlinkMonitorEvent
{
    public ulong Event;
    public ulong Value;
    public ZlinkRoutingId RoutingId;
    public fixed byte LocalAddr[256];
    public fixed byte RemoteAddr[256];
}

[StructLayout(LayoutKind.Sequential)]
internal struct ZlinkPollItemUnix
{
    public IntPtr Socket;
    public int Fd;
    public short Events;
    public short Revents;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ZlinkPollItemWindows
{
    public IntPtr Socket;
    public ulong Fd;
    public short Events;
    public short Revents;
}

[StructLayout(LayoutKind.Sequential)]
internal struct ZlinkPollerEvent
{
    public int MonitorSourceKind;
    public IntPtr Socket;
    public int Fd;
    public IntPtr Timer;
    public IntPtr UserData;
    public short Events;
}

internal static class NativeHelpers
{
    public static unsafe string ReadString(byte* buffer, int maxLen)
    {
        if (buffer == null || maxLen <= 0)
            return string.Empty;
        var len = 0;
        while (len < maxLen && buffer[len] != 0)
            len++;
        if (len == 0)
            return string.Empty;
        return Encoding.UTF8.GetString(new ReadOnlySpan<byte>(buffer, len));
    }

    public static unsafe byte[] ReadRoutingId(ref ZlinkRoutingId id)
    {
        int size = id.Size;
        if (size <= 0)
            return Array.Empty<byte>();
        var data = new byte[size];
        fixed (byte* src = id.Data)
        {
            new ReadOnlySpan<byte>(src, size).CopyTo(data);
        }

        return data;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static unsafe ZlinkRoutingId WriteRoutingId(ReadOnlySpan<byte> routingId)
    {
        if (routingId.Length <= 0 || routingId.Length > 255)
            throw new ArgumentOutOfRangeException(nameof(routingId),
                "routingId length must be between 1 and 255 bytes.");

        ZlinkRoutingId rid = default;
        rid.Size = (byte)routingId.Length;
        for (var i = 0; i < routingId.Length; i++)
            rid.Data[i] = routingId[i];
        return rid;
    }

    public static unsafe string ReadFixedString(byte* buffer, int maxLen)
    {
        return ReadString(buffer, maxLen);
    }

    public static unsafe void WriteFixedString(ReadOnlySpan<char> value,
        byte* destination, int capacity)
    {
        if (destination == null)
            throw new ArgumentNullException(nameof(destination));
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));

        var bytes = new Span<byte>(destination, capacity);
        bytes.Clear();
        if (value.Length == 0)
            return;

        var written = Encoding.UTF8.GetBytes(value, bytes);
        if (written >= capacity)
            throw new ArgumentOutOfRangeException(nameof(value),
                "UTF-8 value exceeds native fixed buffer capacity.");
    }
}