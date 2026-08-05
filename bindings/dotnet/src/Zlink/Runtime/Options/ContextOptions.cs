// SPDX-License-Identifier: MPL-2.0

using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class ContextOptions : IContextOptions
{
    private readonly Context _context;
    private string _threadNamePrefix = string.Empty;

    internal ContextOptions(Context context)
    {
        _context = context;
    }

    public int IoThreads
    {
        get => _context.GetOption(ContextOption.IoThreads);
        set => _context.SetOption(ContextOption.IoThreads, value);
    }

    public int MaxSockets
    {
        get => _context.GetOption(ContextOption.MaxSockets);
        set => _context.SetOption(ContextOption.MaxSockets, value);
    }

    public int SocketLimit => _context.GetOption(ContextOption.SocketLimit);

    public int ThreadPriority
    {
        get => _context.GetOption(ContextOption.ThreadPriority);
        set => _context.SetOption(ContextOption.ThreadPriority, value);
    }

    public int ThreadSchedulingPolicy
    {
        get => _context.GetOption(ContextOption.ThreadSchedPolicy);
        set => _context.SetOption(ContextOption.ThreadSchedPolicy, value);
    }

    public int MaxMessageSize
    {
        get => _context.GetOption(ContextOption.MaxMsgSz);
        set => _context.SetOption(ContextOption.MaxMsgSz, value);
    }

    public int MessageThreadSize => _context.GetOption(ContextOption.MsgTSize);

    public bool Blocky
    {
        get => _context.GetOption(ContextOption.Blocky) != 0;
        set => _context.SetOption(ContextOption.Blocky, value ? 1 : 0);
    }

    public AutoHwmProfile AutoHwmProfile
    {
        get => (AutoHwmProfile)_context.GetOption(ContextOption.AutoHwmProfile);
        set => _context.SetOption(ContextOption.AutoHwmProfile, (int)value);
    }

    public ulong AutoHwmMessageUnitBytes
    {
        get => _context.GetUInt64Option(ContextOption.AutoHwmMsgUnitBytes);
        set => _context.SetUInt64Option(ContextOption.AutoHwmMsgUnitBytes, value);
    }

    public bool AutoHwmEnabled
    {
        get => _context.GetOption(ContextOption.AutoHwmEnabled) != 0;
        set => _context.SetOption(ContextOption.AutoHwmEnabled, value ? 1 : 0);
    }

    public TimeSpan AutoHwmRecalcDebounce
    {
        get => TimeSpan.FromMilliseconds(
            _context.GetOption(ContextOption.AutoHwmRecalcDebounce));
        set => _context.SetOption(ContextOption.AutoHwmRecalcDebounce,
            EncodeMilliseconds(value, nameof(value)));
    }

    public string ThreadNamePrefix
    {
        get => _threadNamePrefix;
        set
        {
            if (value == null)
                throw new ArgumentNullException(nameof(value));
            if (value.IndexOf('\0') >= 0)
                throw new ArgumentException("Value must not contain NUL.",
                    nameof(value));
            if (value.Length > 16)
                throw new ArgumentOutOfRangeException(nameof(value));
            var bytes = Encoding.UTF8.GetBytes(value);
            if (bytes.Length > 16)
                throw new ArgumentOutOfRangeException(nameof(value));
            var rc = NativeMethods.zlink_ctx_set_data(_context.Handle,
                (int)ContextOption.ThreadNamePrefix, bytes, (nuint)bytes.Length);
            ZlinkException.ThrowConfigIfError(rc);
            _threadNamePrefix = value;
        }
    }

    public void AddThreadAffinityCpu(int cpu)
    {
        _context.SetOption(ContextOption.ThreadAffinityCpuAdd, cpu);
    }

    public void RemoveThreadAffinityCpu(int cpu)
    {
        _context.SetOption(ContextOption.ThreadAffinityCpuRemove, cpu);
    }

    private static int EncodeMilliseconds(TimeSpan value, string paramName)
    {
        var millis = value.TotalMilliseconds;
        if (double.IsNaN(millis) || double.IsInfinity(millis)
                                 || millis < 0 || millis > int.MaxValue)
            throw new ArgumentOutOfRangeException(paramName);
        return (int)Math.Ceiling(millis);
    }
}
