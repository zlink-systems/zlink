using System.Reflection;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Adapters;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class RawPortCloseFailureTests
{
    [Fact]
    public async Task ManagedMesh_Rejects_Work_And_Shares_Concurrent_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var router = DispatchProxy.Create<IRouterSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)router;
        proxy.Inner = socket;
        proxy.BlockClose = true;
        var node = new ZLinkManagedMeshNode(context, "blocked-mesh");
        typeof(ZLinkManagedMeshNode)
            .GetField("_socket", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(node, router);
        var first = node.DisposeAsync().AsTask();
        await proxy.CloseAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = node.DisposeAsync().AsTask();
        try
        {
            Assert.Same(first, second);
            Assert.Equal(1, proxy.CloseAttempts);
            Assert.Throws<ObjectDisposedException>(() => node.AddChannel("late-channel"));
        }
        finally
        {
            proxy.ReleaseClose.TrySetResult();
            await Task.WhenAll(first, second);
        }
    }

    [Fact]
    public async Task BackendContext_Rejects_Work_And_Shares_Concurrent_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var wrapped = DispatchProxy.Create<IContext, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)wrapped;
        proxy.Inner = context;
        proxy.BlockClose = true;
        var backend = new ZLinkDotNetBackendRuntimeContext();
        var field = typeof(ZLinkDotNetBackendRuntimeContext).GetField(
            "_context",
            BindingFlags.Instance | BindingFlags.NonPublic
        )!;
        ((IContext)field.GetValue(backend)!).Dispose();
        field.SetValue(backend, wrapped);
        var first = backend.DisposeAsync().AsTask();
        await proxy.CloseAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = backend.DisposeAsync().AsTask();
        try
        {
            Assert.Equal(1, proxy.CloseAttempts);
            Assert.Throws<ObjectDisposedException>(() => backend.CreateDealerSocket());
        }
        finally
        {
            proxy.ReleaseClose.TrySetResult();
        }
        await Task.WhenAll(first, second);
    }

    [Fact]
    public async Task RawRouterPort_Rejects_Work_And_Shares_Concurrent_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var router = DispatchProxy.Create<IRouterSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)router;
        proxy.Inner = socket;
        proxy.BlockClose = true;
        var wrapped = DispatchProxy.Create<IContext, CloseFailureProxy>();
        ((CloseFailureProxy)(object)wrapped).Inner = context;
        ((CloseFailureProxy)(object)wrapped).Router = router;
        var port = new ZLinkRawRouterServicePort(
            wrapped,
            RoutingId.From("blocked"),
            "inproc://blocked"
        );
        var first = port.DisposeAsync().AsTask();
        await proxy.CloseAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = port.DisposeAsync().AsTask();
        try
        {
            Assert.Equal(1, proxy.CloseAttempts);
            Assert.Throws<ObjectDisposedException>(port.Start);
        }
        finally
        {
            proxy.ReleaseClose.TrySetResult();
        }
        await Task.WhenAll(first, second);
    }

    [Fact]
    public async Task ClientRuntime_Preserves_Connection_After_Failed_Close()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateDealerSocket();
        var dealer = DispatchProxy.Create<IDealerSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)dealer;
        proxy.Inner = socket;
        proxy.FailClose = true;
        using var errors = new ZLinkRuntimeErrorSink();
        var runtime = new ZLinkClientServerClientRuntime(
            "events",
            null!,
            null!,
            null!,
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            null!,
            errors
        );
        var type = typeof(ZLinkClientServerClientRuntime).GetNestedType(
            "Connection",
            BindingFlags.NonPublic
        )!;
        var connection = Activator.CreateInstance(
            type,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null,
            [
                "events",
                "tcp://127.0.0.1:1",
                null,
                dealer,
                CancellationToken.None,
                null,
                (Action<bool>)(_ => { }),
                TimeProvider.System,
                errors,
            ],
            null
        )!;
        var connections = (System.Collections.IDictionary)
            typeof(ZLinkClientServerClientRuntime)
                .GetField("_connections", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(runtime)!;
        connections.Add("manual:tcp://127.0.0.1:1", connection);
        await Assert.ThrowsAnyAsync<Exception>(async () => await runtime.DisposeAsync());
        await runtime.DisposeAsync();
        Assert.Equal(2, proxy.CloseAttempts);
    }

    [Fact]
    public async Task AutomaticFanout_Preserves_Failed_Subscriber_Close()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateSubSocket();
        var subscriber = DispatchProxy.Create<ISubSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)subscriber;
        proxy.Inner = socket;
        proxy.FailClose = true;
        var backend = DispatchProxy.Create<IZLinkBackendRuntimeContext, CloseFailureProxy>();
        ((CloseFailureProxy)(object)backend).Subscriber = subscriber;
        using var errors = new ZLinkRuntimeErrorSink();
        var lifecycle = new ZLinkFrameworkHostLifecycleState();
        var registration = new ZLinkFrameworkRegistration();
        registration.Channels.Add(
            "events",
            new ZLinkChannelRegistration
            {
                ChannelName = "events",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration
                {
                    AutomaticDiscoveryEnabled = true,
                },
            }
        );
        var runtime = new ZLinkAutomaticFanoutSubscriberRuntime(
            "events",
            backend,
            new ZLinkSocketConfig(),
            new HashSet<string>(),
            new ZLinkChannelReceiveLoop(null!, null!),
            new ZLinkFanoutRuntimeService(registration, lifecycle),
            errors,
            CancellationToken.None
        );
        var descriptor = new ZLinkFanoutPublisherDescriptor(
            "events",
            RoutingId.From("publisher"),
            1,
            1,
            "tcp://127.0.0.1:1",
            ZLinkFrameworkRuntimeState.Serving,
            "plaintext",
            "owner",
            1,
            default
        );
        await runtime.ReplaceAsync(
            [
                new ZLinkFanoutConnectionPlan(
                    descriptor,
                    true,
                    ZLinkFanoutPublisherConnectionState.Connecting
                ),
            ],
            new ZLinkLocationRuntimeSnapshot("healthy", null, null)
        );
        await proxy.CloseAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Same(
            proxy.Failure,
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await runtime.DisposeAsync()
            )
        );
        await runtime.DisposeAsync();
        Assert.Equal(2, proxy.CloseAttempts);
    }

    [Fact]
    public async Task AutomaticFanout_Rejects_Work_And_Shares_Concurrent_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateSubSocket();
        var subscriber = DispatchProxy.Create<ISubSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)subscriber;
        proxy.Inner = socket;
        proxy.BlockClose = true;
        var backend = DispatchProxy.Create<IZLinkBackendRuntimeContext, CloseFailureProxy>();
        ((CloseFailureProxy)(object)backend).Subscriber = subscriber;
        using var errors = new ZLinkRuntimeErrorSink();
        var lifecycle = new ZLinkFrameworkHostLifecycleState();
        var registration = new ZLinkFrameworkRegistration();
        registration.Channels.Add(
            "events",
            new ZLinkChannelRegistration
            {
                ChannelName = "events",
                AutoConnectType = ZLinkLocationAutoConnectType.Fanout,
                Subscriber = new ZLinkChannelSubscriberCapabilityRegistration
                {
                    AutomaticDiscoveryEnabled = true,
                },
            }
        );
        var runtime = new ZLinkAutomaticFanoutSubscriberRuntime(
            "events",
            backend,
            new ZLinkSocketConfig(),
            new HashSet<string>(),
            new ZLinkChannelReceiveLoop(null!, null!),
            new ZLinkFanoutRuntimeService(registration, lifecycle),
            errors,
            CancellationToken.None
        );
        var descriptor = new ZLinkFanoutPublisherDescriptor(
            "events",
            RoutingId.From("publisher"),
            1,
            1,
            "tcp://127.0.0.1:1",
            ZLinkFrameworkRuntimeState.Serving,
            "plaintext",
            "owner",
            1,
            default
        );
        var type = typeof(ZLinkAutomaticFanoutSubscriberRuntime).GetNestedType(
            "Connection",
            BindingFlags.NonPublic
        )!;
        var connection = Activator.CreateInstance(
            type,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            null,
            [runtime, descriptor],
            null
        )!;
        type.GetField("_socket", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(connection, subscriber);
        var connections = (System.Collections.IDictionary)
            typeof(ZLinkAutomaticFanoutSubscriberRuntime)
                .GetField("_connections", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(runtime)!;
        connections.Add((descriptor.PublisherRid, descriptor.LifecycleGeneration), connection);
        var first = runtime.DisposeAsync().AsTask();
        await proxy.CloseAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = runtime.DisposeAsync().AsTask();
        try
        {
            Assert.Same(first, second);
            Assert.Equal(1, proxy.CloseAttempts);
            await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
                await runtime.ReplaceAsync(
                    [],
                    new ZLinkLocationRuntimeSnapshot("healthy", null, null)
                )
            );
        }
        finally
        {
            proxy.ReleaseClose.TrySetResult();
        }
        await Task.WhenAll(first, second);
    }

    [Fact]
    public async Task ChannelBundle_Preserves_Socket_After_Failed_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var wrapped = DispatchProxy.Create<IAsyncDisposable, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)wrapped;
        proxy.Inner = socket;
        var bundle = new ZLinkChannelRuntimeBundle(wrapped);
        Assert.Same(
            proxy.Failure,
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await bundle.DisposeAsync()
            )
        );
        await bundle.DisposeAsync();
        Assert.Equal(2, proxy.CloseAttempts);
    }

    [Fact]
    public async Task BackendContext_Preserves_Context_After_Failed_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var wrapped = DispatchProxy.Create<IContext, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)wrapped;
        proxy.Inner = context;
        proxy.FailClose = true;
        var backend = new ZLinkDotNetBackendRuntimeContext();
        var field = typeof(ZLinkDotNetBackendRuntimeContext).GetField(
            "_context",
            BindingFlags.Instance | BindingFlags.NonPublic
        )!;
        ((IContext)field.GetValue(backend)!).Dispose();
        field.SetValue(backend, wrapped);
        Assert.Same(
            proxy.Failure,
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await backend.DisposeAsync()
            )
        );
        await backend.DisposeAsync();
        Assert.Equal(2, proxy.CloseAttempts);
    }

    [Fact]
    public async Task ManagedMeshNode_Preserves_Socket_After_Failed_Disposal()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var router = DispatchProxy.Create<IRouterSocket, CloseFailureProxy>();
        var proxy = (CloseFailureProxy)(object)router;
        proxy.Inner = socket;
        var node = new ZLinkManagedMeshNode(context, "close-failure");
        typeof(ZLinkManagedMeshNode)
            .GetField("_socket", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(node, router);
        Assert.Same(
            proxy.Failure,
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await node.DisposeAsync()
            )
        );
        await node.DisposeAsync();
        Assert.Equal(2, proxy.CloseAttempts);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RawRouterPort_Preserves_Socket_After_Failed_Disposal(bool asynchronous)
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var router = DispatchProxy.Create<IRouterSocket, CloseFailureProxy>();
        var socketProxy = (CloseFailureProxy)(object)router;
        socketProxy.Inner = socket;
        var wrappedContext = DispatchProxy.Create<IContext, CloseFailureProxy>();
        var contextProxy = (CloseFailureProxy)(object)wrappedContext;
        contextProxy.Inner = context;
        contextProxy.Router = router;
        var port = new ZLinkRawRouterServicePort(
            wrappedContext,
            RoutingId.From("close-failure"),
            "inproc://close-failure"
        );

        if (asynchronous)
        {
            var failure = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await port.DisposeAsync()
            );
            Assert.Same(socketProxy.Failure, failure);
            await port.DisposeAsync();
        }
        else
        {
            Assert.Same(
                socketProxy.Failure,
                Assert.Throws<InvalidOperationException>(port.Dispose)
            );
            port.Dispose();
        }
        Assert.Equal(2, socketProxy.CloseAttempts);
        port.Dispose();
        Assert.Equal(2, socketProxy.CloseAttempts);
    }

    public class CloseFailureProxy : DispatchProxy
    {
        public object Inner { get; set; } = null!;
        public IRouterSocket? Router { get; set; }
        public ISubSocket? Subscriber { get; set; }
        public TaskCompletionSource CloseAttempted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool BlockClose { get; set; }
        public TaskCompletionSource ReleaseClose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool FailClose { get; set; }
        public int CloseAttempts { get; private set; }
        public Exception Failure { get; } = new InvalidOperationException("First close failed.");

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            var method = targetMethod!;
            if (method.Name == nameof(IContext.CreateRouterSocket))
                return Router;
            if (method.Name == nameof(IZLinkBackendRuntimeContext.CreateSubscriberSocket))
                return Subscriber;
            if (BlockClose && method.Name is "Dispose" or "DisposeAsync")
            {
                ++CloseAttempts;
                CloseAttempted.TrySetResult();
                if (method.Name == "DisposeAsync")
                    return new ValueTask(ReleaseClose.Task);
                ReleaseClose.Task.GetAwaiter().GetResult();
                return null;
            }
            if ((FailClose || Inner is IRouterSocket) && method.Name is "Dispose" or "DisposeAsync")
            {
                if (++CloseAttempts == 1)
                {
                    CloseAttempted.TrySetResult();
                    if (method.Name == "DisposeAsync")
                        return ValueTask.FromException(Failure);
                    throw Failure;
                }
            }
            try
            {
                return method.Invoke(Inner, args);
            }
            catch (TargetInvocationException error) when (error.InnerException is not null)
            {
                System
                    .Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error.InnerException)
                    .Throw();
                throw;
            }
        }
    }
}
