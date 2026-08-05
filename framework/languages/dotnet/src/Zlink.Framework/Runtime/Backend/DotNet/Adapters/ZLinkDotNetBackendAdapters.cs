using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

namespace Zlink.Framework.Runtime.Backend.DotNet.Adapters;

internal sealed class ZLinkDotNetChannelBackendAdapter : IZLinkChannelBackendAdapter
{
    public IZLinkBackendContext CreateContext()
    {
        var context = Systems.Zlink.Zlink.CreateContext();
        try
        {
            context.Options.Blocky = false;
            return new ZLinkBackendContextWrapper(context);
        }
        catch
        {
            context.Dispose();
            throw;
        }
    }

    public void ConfigureAutoHwm(
        IZLinkBackendContext context,
        ZLinkApplicationHwmProfile profile)
    {
        var nativeContext = RequireContext(context).NativeContext;
        nativeContext.Options.AutoHwmProfile = profile switch
        {
            ZLinkApplicationHwmProfile.Compact => AutoHwmProfile.Compact,
            ZLinkApplicationHwmProfile.LowLatency => AutoHwmProfile.LowLatency,
            ZLinkApplicationHwmProfile.Balanced => AutoHwmProfile.Balanced,
            ZLinkApplicationHwmProfile.Throughput => AutoHwmProfile.Throughput,
            _ => throw new ZLinkConfigurationException(
                $"Unknown ApplicationHwmProfile value '{(int)profile}'.")
        };
        nativeContext.Options.AutoHwmEnabled = true;
    }

    public IZLinkBackendDealerSocket CreateDealerSocket(IZLinkBackendContext context)
    {
        var socket = RequireContext(context).NativeContext.CreateDealerSocket();
        return new ZLinkBackendDealerSocketWrapper(
            socket);
    }

    public IZLinkBackendRouterSocket CreateRouterSocket(IZLinkBackendContext context)
    {
        var socket = RequireContext(context).NativeContext.CreateRouterSocket();
        return new ZLinkBackendRouterSocketWrapper(
            socket);
    }

    public IZLinkBackendPublisherSocket CreatePublisherSocket(IZLinkBackendContext context)
    {
        var socket = RequireContext(context).NativeContext.CreatePubSocket();
        return new ZLinkBackendPublisherSocketWrapper(
            socket);
    }

    public IZLinkBackendSubscriberSocket CreateSubscriberSocket(IZLinkBackendContext context)
    {
        var socket = RequireContext(context).NativeContext.CreateSubSocket();
        return new ZLinkBackendSubscriberSocketWrapper(
            socket);
    }

    private static ZLinkBackendContextWrapper RequireContext(IZLinkBackendContext context) =>
        context as ZLinkBackendContextWrapper
        ?? throw new InvalidOperationException("Expected the .NET backend context wrapper.");
}

internal sealed class ZLinkDotNetSpotBackendAdapter : IZLinkSpotBackendAdapter
{
    public IZLinkBackendSpotNode CreateSpotNode(IZLinkBackendContext context, string meshName)
    {
        var nativeContext = (context as ZLinkBackendContextWrapper)?.NativeContext
                            ?? throw new InvalidOperationException("Expected the .NET backend context wrapper.");
        return new ZLinkBackendSpotNodeWrapper(
            new ZLinkManagedMeshNode(nativeContext, meshName));
    }
}

internal sealed class ZLinkDotNetStreamBackendAdapter : IZLinkStreamBackendAdapter
{
    public IZLinkBackendStreamSocket CreateStreamSocket(
        IZLinkBackendContext context,
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null)
    {
        var nativeContext = (context as ZLinkBackendContextWrapper)?.NativeContext
                            ?? throw new InvalidOperationException(
                                "Expected the .NET backend context wrapper.");
        var socket = nativeContext.CreateStreamSocket();
        socket.Options.Linger = TimeSpan.Zero;

        // Actor-dispatch STREAM nodes reuse the framework's single MeshNode
        // (spec 31 §2), whose dispatch pump drains the bind/unbind completions the
        // stream wrapper awaits. Only when no MeshNode exists is a standalone node
        // minted (no completion table, so bind/unbind stays best-effort there).
        if (actorDispatchNode is ZLinkBackendSpotNodeWrapper shared)
            return new ZLinkBackendStreamSocketWrapper(
                socket, shared.NativeNode, shared.Completions, ownsNode: false);

        var node = new ZLinkManagedMeshNode(nativeContext, standaloneMeshName);
        node.SetRoutingId(RoutingId.From(Guid.NewGuid()));
        node.SetBind($"inproc://zlink-stream-{Guid.NewGuid():N}");
        node.Start();
        return new ZLinkBackendStreamSocketWrapper(
            socket, node, completions: null, ownsNode: true);
    }
}

internal sealed class ZLinkDotNetMonitoringBackendAdapter : IZLinkMonitoringBackendAdapter
{
    public IZLinkBackendSocketMonitor OpenSocketMonitor(IZLinkBackendSocket socket)
    {
        var nativeMonitor = socket switch
        {
            ZLinkBackendDealerSocketWrapper dealer => dealer.NativeSocket.MonitorOpen(),
            ZLinkBackendRouterSocketWrapper router => router.NativeSocket.MonitorOpen(),
            ZLinkBackendPublisherSocketWrapper publisher => publisher.NativeSocket.MonitorOpen(),
            ZLinkBackendSubscriberSocketWrapper subscriber => subscriber.NativeSocket.MonitorOpen(),
            ZLinkBackendStreamSocketWrapper stream => stream.NativeSocket.MonitorOpen(),
            _ => throw new InvalidOperationException("Expected a .NET backend socket wrapper.")
        };
        return new ZLinkBackendSocketMonitorWrapper(nativeMonitor);
    }
}
