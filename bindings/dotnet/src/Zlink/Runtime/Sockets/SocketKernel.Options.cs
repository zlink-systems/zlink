// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void SetOption(SocketOptionKey<int> option, int value)
    {
        EnsureOptionSupported(option.Option);
        _options.SetInt32(option.Option, value);
    }

    public void SetOption(SocketOptionKey<long> option, long value)
    {
        EnsureOptionSupported(option.Option);
        _options.SetInt64(option.Option, value);
    }

    public void SetOption(SocketOptionKey<ulong> option, ulong value)
    {
        EnsureOptionSupported(option.Option);
        _options.SetUInt64(option.Option, value);
    }

    public void SetOption(SocketOptionKey<byte[]> option, byte[] value)
    {
        EnsureOptionSupported(option.Option);
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        SetOption(option, value.AsSpan());
    }

    public void SetOption(SocketOptionKey<byte[]> option, ReadOnlySpan<byte> value)
    {
        EnsureOptionSupported(option.Option);
        _options.SetBytes(option.Option, value);
    }

    public void SetOption(SocketOptionKey<string> option, string value)
    {
        EnsureOptionSupported(option.Option);
        _options.SetString(option.Option, value);
    }

    public int GetOption(SocketOptionKey<int> option)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetInt32(option.Option);
    }

    public long GetOption(SocketOptionKey<long> option)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetInt64(option.Option);
    }

    public ulong GetOption(SocketOptionKey<ulong> option)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetUInt64(option.Option);
    }

    public byte[] GetOption(SocketOptionKey<byte[]> option, int initialSize = 256)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetBytes(option.Option, initialSize);
    }

    public int GetOption(SocketOptionKey<byte[]> option, Span<byte> destination)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetBytesInto(option.Option, destination);
    }

    public string GetOption(SocketOptionKey<string> option, int initialSize = 256)
    {
        EnsureOptionSupported(option.Option);
        return _options.GetString(option.Option, initialSize);
    }

    public SocketMonitor MonitorOpen(SocketEvent events = SocketEvent.All)
    {
        ZlinkSocketMonitorOpenOptions options = new()
        {
            Events = (uint)events
        };
        var handle = NativeMethods.zlink_socket_monitor_open(Handle, in options);
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(
                NativeMethods.zlink_errno());
        return new SocketMonitor(handle);
    }
}
