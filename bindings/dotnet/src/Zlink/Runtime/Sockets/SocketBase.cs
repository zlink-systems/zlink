// SPDX-License-Identifier: MPL-2.0

using System.ComponentModel;
using Systems.Zlink.Runtime.Native;
using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

[EditorBrowsable(EditorBrowsableState.Never)]
internal abstract class SocketBase : ISocket, ISocketOptionEndpoint
{
    internal SocketBase(Context context, SocketType type)
    {
        Kernel = new SocketKernel(context, type);
        Options = new CommonSocketOptions(this);
    }

    internal SocketBase(SocketKernel kernel)
    {
        Kernel = kernel ?? throw new ArgumentNullException(nameof(kernel));
        Options = new CommonSocketOptions(this);
    }

    internal IntPtr Handle => Kernel.Handle;
    internal SocketKernel Kernel { get; }

    internal object SubmitGate { get; } = new();
    public CommonSocketOptions Options { get; }

    public void Bind(string address)
    {
        Kernel.Bind(address);
    }

    public void Unbind(string address)
    {
        Kernel.Unbind(address);
    }

    public ISocketMonitor MonitorOpen(SocketEvent events = SocketEvent.All)
    {
        EnumValidation.EnsureSocketEvents(events, nameof(events));
        try
        {
            return Kernel.MonitorOpen(events);
        }
        catch (ZlinkException ex)
        {
            throw ZlinkException.CreateConfigException(ex.NativeErrno);
        }
    }

    public void SetTlsServer(string certPath, string keyPath,
        bool requireClientCert = false)
    {
        if (certPath == null)
            throw new ArgumentNullException(nameof(certPath));
        if (keyPath == null)
            throw new ArgumentNullException(nameof(keyPath));

        var rc = NativeMethods.zlink_set_tls_server(Handle, certPath, keyPath,
            requireClientCert ? 1 : 0);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    public void SetTlsClient(string caCertPath, string hostname,
        bool trustSystem = false)
    {
        if (caCertPath == null)
            throw new ArgumentNullException(nameof(caCertPath));
        if (hostname == null)
            throw new ArgumentNullException(nameof(hostname));

        var rc = NativeMethods.zlink_set_tls_client(Handle, caCertPath, hostname,
            trustSystem ? 1 : 0);
        if (rc != 0)
            throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
    }

    public void Close()
    {
        Kernel.Close();
    }

    public void Dispose()
    {
        try
        {
            Kernel.Dispose();
        }
        catch (ZlinkException ex)
        {
            throw ZlinkException.CreateCloseException(ex.NativeErrno);
        }

        GC.SuppressFinalize(this);
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<int> option, int value)
    {
        Kernel.SetOption(option, value);
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<long> option,
        long value)
    {
        Kernel.SetOption(option, value);
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<ulong> option,
        ulong value)
    {
        Kernel.SetOption(option, value);
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<byte[]> option,
        byte[] value)
    {
        Kernel.SetOption(option, value);
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<byte[]> option,
        ReadOnlySpan<byte> value)
    {
        Kernel.SetOption(option, value);
    }

    void ISocketOptionEndpoint.SetOption(SocketOptionKey<string> option,
        string value)
    {
        Kernel.SetOption(option, value);
    }

    int ISocketOptionEndpoint.GetOption(SocketOptionKey<int> option)
    {
        return Kernel.GetOption(option);
    }

    long ISocketOptionEndpoint.GetOption(SocketOptionKey<long> option)
    {
        return Kernel.GetOption(option);
    }

    ulong ISocketOptionEndpoint.GetOption(SocketOptionKey<ulong> option)
    {
        return Kernel.GetOption(option);
    }

    byte[] ISocketOptionEndpoint.GetOption(SocketOptionKey<byte[]> option,
        int initialSize)
    {
        return Kernel.GetOption(option, initialSize);
    }

    int ISocketOptionEndpoint.GetOption(SocketOptionKey<byte[]> option,
        Span<byte> destination)
    {
        return Kernel.GetOption(option, destination);
    }

    string ISocketOptionEndpoint.GetOption(SocketOptionKey<string> option,
        int initialSize)
    {
        return Kernel.GetOption(option, initialSize);
    }

    SocketType ISocketOptionEndpoint.SocketType => Kernel.Type;

}
