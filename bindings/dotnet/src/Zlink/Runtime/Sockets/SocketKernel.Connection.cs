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
            throw ZlinkException.CreateBindException(
                NativeMethods.GetLastPInvokeError());
    }

    public void Connect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_connect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(
                NativeMethods.GetLastPInvokeError());
    }

    public void Unbind(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_unbind(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(
                NativeMethods.GetLastPInvokeError());
    }

    public void Disconnect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_disconnect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException(
                NativeMethods.GetLastPInvokeError());
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        var nativeRid = peerRid.ToNative();
        var rc = NativeMethods.zlink_disconnect_rid(Handle, ref nativeRid);
        ZlinkException.ThrowConnectIfError(rc);
    }

    public unsafe IReadOnlyList<RouterRoute> RoutesSnapshot()
    {
        // Core ROUTER §10.1: BUFFER_TOO_SMALL reports the required row count
        // and keeps POLLROUTE readiness, so retry with that capacity.
        var rows = new ZlinkRouterRoute[InitialRouteSnapshotCapacity];
        while (true)
        {
            int rc;
            nuint count;
            fixed (ZlinkRouterRoute* routes = rows)
                rc = NativeMethods.zlink_router_routes_snapshot(Handle, routes,
                    (nuint)rows.Length, out count);
            if ((ConfigResult)rc == ConfigResult.BufferTooSmall
                && count > (nuint)rows.Length)
            {
                rows = new ZlinkRouterRoute[checked((int)count)];
                continue;
            }
            ZlinkException.ThrowConfigIfError(rc);
            if (count > (nuint)rows.Length)
                throw ZlinkException.CreateConfigException(
                    ConfigResult.InternalError);
            var result = new RouterRoute[(int)count];
            for (var index = 0; index < result.Length; index++)
                result[index] = new RouterRoute(
                    RoutingId.From(NativeHelpers.ReadRoutingId(ref rows[index].Rid)),
                    rows[index].RouteGeneration);
            return result;
        }
    }

    private const int InitialRouteSnapshotCapacity = 16;

}
