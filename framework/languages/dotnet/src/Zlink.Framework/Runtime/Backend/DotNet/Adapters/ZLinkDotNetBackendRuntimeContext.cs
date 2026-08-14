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
    private ZLinkApplicationJobQueue? _applicationJobQueue;
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

    public void ConfigureCoreHwm(
        AutoHwmProfile profile,
        ulong memoryLimitBytes,
        ulong budgetBytes)
    {
        ThrowIfDisposed();
        _context.Options.CoreHwmProfile = profile;
        _context.Options.CoreHwmMemoryLimitBytes = memoryLimitBytes;
        _context.Options.CoreHwmBudgetBytes = budgetBytes;
        _context.Options.AutoHwmEnabled = true;
        _context.RecalculateAutoHwm();
    }

    public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot()
    {
        ThrowIfDisposed();
        return _context.GetCoreHwmBudgetSnapshot();
    }

    public void ResetCoreHwmBudgetMetrics()
    {
        ThrowIfDisposed();
        _context.ResetCoreHwmBudgetMetrics();
    }

    public void ConfigureApplicationJobQueue(
        ZLinkApplicationJobQueue applicationJobQueue)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(applicationJobQueue);
        if (Interlocked.CompareExchange(
                ref _applicationJobQueue,
                applicationJobQueue,
                null) is not null)
            throw new InvalidOperationException(
                "The Application Job Queue is already configured.");
    }

    public IDealerSocket CreateDealerSocket()
    {
        ThrowIfDisposed();
        return _context.CreateDealerSocket();
    }

    public IRouterSocket CreateRouterSocket()
    {
        ThrowIfDisposed();
        return _context.CreateRouterSocket();
    }

    public IPubSocket CreatePublisherSocket()
    {
        ThrowIfDisposed();
        return _context.CreatePubSocket();
    }

    public ISubSocket CreateSubscriberSocket()
    {
        ThrowIfDisposed();
        return _context.CreateSubSocket();
    }

    public IZLinkBackendSpotNode CreateSpotNode(string meshName)
    {
        ThrowIfDisposed();
        return new ZLinkBackendSpotNodeWrapper(
            new ZLinkManagedMeshNode(
                _context,
                meshName,
                applicationJobQueue: _applicationJobQueue),
            _applicationJobQueue);
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

        var node = new ZLinkManagedMeshNode(
            _context,
            standaloneMeshName,
            applicationJobQueue: _applicationJobQueue);
        var completions = new ZLinkMeshCompletionTable();
        var completionPump = new ZLinkMeshDispatchPump(
            node,
            completions,
            _applicationJobQueue);
        node.SetRoutingId(RoutingId.From(Guid.NewGuid()));
        node.SetBind($"inproc://zlink-stream-{Guid.NewGuid():N}");
        node.Start();
        completionPump.EnsureStarted();
        return new ZLinkBackendStreamSocketWrapper(
            socket,
            node,
            completions,
            ownsNode: true,
            ownedCompletionPump: completionPump);
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
        IAsyncDisposable socket)
    {
        var nativeMonitor = socket switch
        {
            IDealerSocket dealer => dealer.MonitorOpen(),
            IRouterSocket router => router.MonitorOpen(),
            IPubSocket publisher => publisher.MonitorOpen(),
            ISubSocket subscriber => subscriber.MonitorOpen(),
            ZLinkBackendStreamSocketWrapper stream => stream.NativeSocket.MonitorOpen(),
            _ => throw new InvalidOperationException(
                "Expected a supported .NET binding socket.")
        };
        return new ZLinkBackendSocketMonitorWrapper(nativeMonitor);
    }
}
