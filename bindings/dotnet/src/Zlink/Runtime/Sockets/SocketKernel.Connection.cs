// SPDX-License-Identifier: MPL-2.0

using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void Bind(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_bind(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateBindException((BindResult)rc);
    }

    public void Connect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_connect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException((ConnectResult)rc);
    }

    public void Unbind(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_unbind(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException((ConnectResult)rc);
    }

    public void Disconnect(string address)
    {
        BoundaryValidation.ValidateFixedUtf8(address, nameof(address));

        var rc = NativeMethods.zlink_disconnect(Handle, address);
        if (rc != 0)
            throw ZlinkException.CreateConnectException((ConnectResult)rc);
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
        var rows = new ZlinkRouterRoute[InitialSnapshotRowCapacity];
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
                throw new ZlinkConfigException(ConfigResult.InternalError);
            var result = new RouterRoute[(int)count];
            for (var index = 0; index < result.Length; index++)
                result[index] = new RouterRoute(
                    RoutingId.From(NativeHelpers.ReadRoutingId(ref rows[index].Rid)),
                    rows[index].RouteGeneration);
            return result;
        }
    }

    public unsafe IReadOnlyList<ConnectionAttempt> ConnectionAttemptsSnapshot()
    {
        var rows = new ZlinkConnectionAttempt[InitialSnapshotRowCapacity];
        var endpointBytes = new byte[InitialConnectionEndpointBytesCapacity];
        while (true)
        {
            int rc;
            nuint count;
            nuint bytesUsed;
            fixed (ZlinkConnectionAttempt* attempts = rows)
            fixed (byte* endpoints = endpointBytes)
                rc = NativeMethods.zlink_socket_connection_attempts_snapshot(
                    Handle, attempts, (nuint)rows.Length, out count,
                    endpoints, (nuint)endpointBytes.Length, out bytesUsed);
            // Core reports both required capacities without consuming readiness.
            // Only the final successful call supplies a coherent row/arena pair.
            if ((ConfigResult)rc == ConfigResult.BufferTooSmall
                && (count > (nuint)rows.Length || bytesUsed > (nuint)endpointBytes.Length))
            {
                if (count > (nuint)rows.Length)
                    rows = new ZlinkConnectionAttempt[checked((int)count)];
                if (bytesUsed > (nuint)endpointBytes.Length)
                    endpointBytes = new byte[checked((int)bytesUsed)];
                continue;
            }
            ZlinkException.ThrowConfigIfError(rc);
            if (count > (nuint)rows.Length || bytesUsed > (nuint)endpointBytes.Length)
                throw new ZlinkConfigException(ConfigResult.InternalError);
            var result = new ConnectionAttempt[(int)count];
            for (var index = 0; index < result.Length; index++)
            {
                ref var row = ref rows[index];
                if (row.EndpointOffset > bytesUsed
                    || row.EndpointLength > bytesUsed - row.EndpointOffset)
                    throw new ZlinkConfigException(ConfigResult.InternalError);
                result[index] = new ConnectionAttempt(
                    Encoding.UTF8.GetString(endpointBytes, checked((int)row.EndpointOffset),
                        checked((int)row.EndpointLength)),
                    row.IntentId, row.Revision, row.LastFailureId, row.State,
                    new ConnectionFailure(row.LastFailure.Kind, row.LastFailure.Stage,
                        row.LastFailure.NormalizedErrno, row.LastFailure.DisconnectReason));
            }
            return result;
        }
    }

    private const int InitialConnectionEndpointBytesCapacity = 1024;

    private const int InitialSnapshotRowCapacity = 16;

}
