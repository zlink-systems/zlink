// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class Context : NativeOwner, IContext
{
    public Context() : base(CreateHandle())
    {
        Options = new ContextOptions(this);
    }

    internal IntPtr Handle => _handle;

    public ContextOptions Options { get; }

    IContextOptions IContext.Options => Options;

    public IPairSocket CreatePairSocket()
    {
        return new PairSocket(this);
    }

    public IDealerSocket CreateDealerSocket()
    {
        return new DealerSocket(this);
    }

    public IRouterSocket CreateRouterSocket()
    {
        return new RouterSocket(this);
    }

    public IPubSocket CreatePubSocket()
    {
        return new PubSocket(this);
    }

    public ISubSocket CreateSubSocket()
    {
        return new SubSocket(this);
    }

    public IXPubSocket CreateXPubSocket()
    {
        return new XPubSocket(this);
    }

    public IXSubSocket CreateXSubSocket()
    {
        return new XSubSocket(this);
    }

    public IStreamSocket CreateStreamSocket()
    {
        return new StreamSocket(this);
    }

    public void Shutdown()
    {
        EnsureNotDisposed();
        var rc = NativeMethods.zlink_ctx_shutdown(Handle);
        if (rc < 0)
            throw ZlinkException.CreateCloseException(NativeMethods.zlink_errno());
    }

    public void RecalculateAutoHwm()
    {
        EnsureNotDisposed();
        var rc = NativeMethods.zlink_ctx_auto_hwm_recalculate(Handle);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    public void Dispose()
    {
        if (IsClosed)
            return;

        _ = RetryWhileInterrupted(
            () => NativeMethods.zlink_ctx_shutdown(Handle), out _);
        var rc = RetryWhileInterrupted(
            () => NativeMethods.zlink_ctx_term(Handle), out var errno);
        MarkClosed();
        if (rc < 0)
            throw ZlinkException.CreateCloseException(errno);
        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    internal void SetOption(ContextOption option, int value)
    {
        EnsureNotDisposed();
        EnumValidation.EnsureContextOption(option, nameof(option));
        var rc = NativeMethods.zlink_ctx_set(Handle, (int)option, value);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    internal int GetOption(ContextOption option)
    {
        EnsureNotDisposed();
        EnumValidation.EnsureContextOption(option, nameof(option));
        var value = NativeMethods.zlink_ctx_get(Handle, (int)option,
            out var errorOut);
        if (errorOut != (int)ConfigResult.Ok)
            throw new ZlinkConfigException((ConfigResult)errorOut,
                NativeMethods.zlink_errno());
        return value;
    }

    internal void SetUInt64Option(ContextOption option, ulong value)
    {
        EnsureNotDisposed();
        EnumValidation.EnsureContextOption(option, nameof(option));
        var rc = NativeMethods.zlink_ctx_set_data(Handle, (int)option,
            in value, sizeof(ulong));
        ZlinkException.ThrowConfigIfError(rc);
    }

    internal ulong GetUInt64Option(ContextOption option)
    {
        EnsureNotDisposed();
        EnumValidation.EnsureContextOption(option, nameof(option));
        nuint size = sizeof(ulong);
        var rc = NativeMethods.zlink_ctx_get_data(Handle, (int)option,
            out var value, ref size);
        ZlinkException.ThrowConfigIfError(rc);
        if (size != sizeof(ulong))
            throw new InvalidOperationException(
                "The native context option returned an invalid value size.");
        return value;
    }

    ~Context()
    {
        if (IsClosed)
            return;

        try
        {
            _ = NativeMethods.zlink_ctx_shutdown(Handle);
        }
        catch
        {
        }
    }

    private void EnsureNotDisposed()
    {
        EnsureNativeHandle(nameof(Context));
    }

    private static IntPtr CreateHandle()
    {
        var handle = NativeMethods.zlink_ctx_new();
        if (handle == IntPtr.Zero)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
        return handle;
    }
}
