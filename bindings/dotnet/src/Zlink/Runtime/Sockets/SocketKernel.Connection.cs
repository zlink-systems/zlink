// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void Bind(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_bind(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateBindException(NativeMethods.zlink_errno());
    }

    public void Connect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_connect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(NativeMethods.zlink_errno());
    }

    public void Unbind(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_unbind(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(NativeMethods.zlink_errno());
    }

    public void Disconnect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_disconnect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(NativeMethods.zlink_errno());
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        var nativeRid = peerRid.ToNative();
        var rc = NativeMethods.zlink_disconnect_rid(Handle, ref nativeRid);
        ZlinkException.ThrowConnectIfError(rc);
    }

}
