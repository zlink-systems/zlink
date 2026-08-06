using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

namespace Zlink.Framework.Runtime.Backend.DotNet.Adapters;

/// <summary>
/// Owns one binding context and the Framework backend-resource creation policy.
/// The binding context never crosses this port into semantic runtime code.
/// </summary>
internal sealed class ZLinkDotNetBackendRuntimeContext
    : IZLinkBackendRuntimeContext
{
    private readonly IContext _context;
    private int _disposed;

    public ZLinkDotNetBackendRuntimeContext()
    {
        _context = Systems.Zlink.Zlink.CreateContext();
        try
        {
            _context.Options.Blocky = false;
        }
        catch
        {
            _context.Dispose();
            throw;
        }
    }

    public void ConfigureAutoHwm(ZLinkApplicationHwmProfile profile)
    {
        ThrowIfDisposed();
        _context.Options.AutoHwmProfile = profile switch
        {
            ZLinkApplicationHwmProfile.Compact => AutoHwmProfile.Compact,
            ZLinkApplicationHwmProfile.LowLatency => AutoHwmProfile.LowLatency,
            ZLinkApplicationHwmProfile.Balanced => AutoHwmProfile.Balanced,
            ZLinkApplicationHwmProfile.Throughput => AutoHwmProfile.Throughput,
            _ => throw new ZLinkConfigurationException(
                $"Unknown ApplicationHwmProfile value '{(int)profile}'.")
        };
        _context.Options.AutoHwmEnabled = true;
    }

    public IZLinkBackendDealerSocket CreateDealerSocket()
    {
        ThrowIfDisposed();
        return new ZLinkBackendDealerSocketWrapper(
            _context.CreateDealerSocket());
    }

    public IZLinkBackendRouterSocket CreateRouterSocket()
    {
        ThrowIfDisposed();
        return new ZLinkBackendRouterSocketWrapper(
            _context.CreateRouterSocket());
    }

    public IZLinkBackendPublisherSocket CreatePublisherSocket()
    {
        ThrowIfDisposed();
        return new ZLinkBackendPublisherSocketWrapper(
            _context.CreatePubSocket());
    }

    public IZLinkBackendSubscriberSocket CreateSubscriberSocket()
    {
        ThrowIfDisposed();
        return new ZLinkBackendSubscriberSocketWrapper(
            _context.CreateSubSocket());
    }

    public IZLinkBackendSpotNode CreateSpotNode(string meshName)
    {
        ThrowIfDisposed();
        return new ZLinkBackendSpotNodeWrapper(
            new ZLinkManagedMeshNode(_context, meshName));
    }

    public IZLinkBackendStreamSocket CreateStreamSocket(
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null)
    {
        ThrowIfDisposed();
        var socket = _context.CreateStreamSocket();
        socket.Options.Linger = TimeSpan.Zero;

        // Actor-dispatch STREAM nodes reuse the Framework's single MeshNode.
        // Only a stream without a shared node creates and owns its standalone
        // node; the ownership decision remains at this integration boundary.
        if (actorDispatchNode is ZLinkBackendSpotNodeWrapper shared)
            return new ZLinkBackendStreamSocketWrapper(
                socket, shared.NativeNode, shared.Completions, ownsNode: false);

        var node = new ZLinkManagedMeshNode(_context, standaloneMeshName);
        node.SetRoutingId(RoutingId.From(Guid.NewGuid()));
        node.SetBind($"inproc://zlink-stream-{Guid.NewGuid():N}");
        node.Start();
        return new ZLinkBackendStreamSocketWrapper(
            socket, node, completions: null, ownsNode: true);
    }

    public ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return ValueTask.CompletedTask;
        return _context.DisposeAsync();
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref _disposed) != 0,
            this);
    }
}

internal sealed class ZLinkDotNetMonitoringBackendAdapter
    : IZLinkMonitoringBackendAdapter
{
    public IZLinkBackendSocketMonitor OpenSocketMonitor(
        IZLinkBackendSocket socket)
    {
        var nativeMonitor = socket switch
        {
            ZLinkBackendDealerSocketWrapper dealer => dealer.NativeSocket.MonitorOpen(),
            ZLinkBackendRouterSocketWrapper router => router.NativeSocket.MonitorOpen(),
            ZLinkBackendPublisherSocketWrapper publisher => publisher.NativeSocket.MonitorOpen(),
            ZLinkBackendSubscriberSocketWrapper subscriber => subscriber.NativeSocket.MonitorOpen(),
            ZLinkBackendStreamSocketWrapper stream => stream.NativeSocket.MonitorOpen(),
            _ => throw new InvalidOperationException(
                "Expected a .NET backend socket wrapper.")
        };
        return new ZLinkBackendSocketMonitorWrapper(nativeMonitor);
    }
}
