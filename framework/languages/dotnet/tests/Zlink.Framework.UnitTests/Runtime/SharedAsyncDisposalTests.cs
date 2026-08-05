using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Streams;
using System.Reflection;

namespace Zlink.Framework.UnitTests;

public sealed class SharedAsyncDisposalTests
{
    [Fact]
    public async Task AutoConnectHost_Repeated_Dispose_Callers_Share_Finalization()
    {
        var host = new ZLinkLocationAutoConnectHost(
            null!,
            null!,
            new ZLinkLocationOptions(),
            store: new ZLinkInMemoryLocationStore());
        var first = host.DisposeAsync().AsTask();
        var second = host.DisposeAsync().AsTask();
        Assert.Same(first, second);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task LocationStoreOwner_Repeated_Dispose_Callers_Share_Finalization()
    {
        var owner = new ZLinkLocationStoreInstanceOwner(null!);
        var first = owner.DisposeAsync().AsTask();
        var second = owner.DisposeAsync().AsTask();
        Assert.Same(first, second);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task LocationStoreOwner_Disposes_Dependents_Before_The_Store()
    {
        var store = DispatchProxy.Create<ITrackedLocationStore, TrackedLocationStoreProxy>();
        var tracker = (TrackedLocationStoreProxy)(object)store;
        var dependent = new StoreUsingDisposable(tracker);
        var owner = new ZLinkLocationStoreInstanceOwner(store);
        owner.RegisterBeforeStoreDispose(dependent);

        await owner.DisposeAsync();

        Assert.Equal(1, dependent.DisposeCount);
        Assert.Equal(1, tracker.DisposeCount);
        Assert.True(dependent.ObservedUsableStore);
    }

    [Fact]
    public async Task LocationStoreOwner_Disposes_Distinct_Provider_Stores_Once()
    {
        var locationStore =
            DispatchProxy.Create<ITrackedLocationStore, TrackedLocationStoreProxy>();
        var relocationStore =
            DispatchProxy.Create<ITrackedRelocationStore, TrackedRelocationStoreProxy>();
        var locationTracker = (TrackedLocationStoreProxy)(object)locationStore;
        var relocationTracker = (TrackedRelocationStoreProxy)(object)relocationStore;
        var owner = new ZLinkLocationStoreInstanceOwner(locationStore, relocationStore);

        await owner.DisposeAsync();

        Assert.Equal(1, locationTracker.DisposeCount);
        Assert.Equal(1, relocationTracker.DisposeCount);
    }

    [Fact]
    public async Task LocationStoreOwner_Disposes_Shared_Provider_Store_Once()
    {
        var store = DispatchProxy.Create<ITrackedProviderStore, TrackedProviderStoreProxy>();
        var tracker = (TrackedProviderStoreProxy)(object)store;
        var owner = new ZLinkLocationStoreInstanceOwner(store, store);

        await owner.DisposeAsync();

        Assert.Equal(1, tracker.DisposeCount);
    }

    [Fact]
    public async Task SerialQueue_Concurrent_Dispose_Callers_Share_The_Drain()
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var errors = new ZLinkRuntimeErrorSink();
        var queue = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(errors, CancellationToken.None),
            errors,
            CancellationToken.None);
        Assert.True(queue.TryPost(async _ =>
        {
            entered.TrySetResult();
            await release.Task.ConfigureAwait(false);
        }, out _));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var first = queue.DisposeAsync().AsTask();
        var second = queue.DisposeAsync().AsTask();
        Assert.Same(first, second);
        Assert.False(second.IsCompleted);

        release.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ChannelBundle_Concurrent_Dispose_Callers_Share_Socket_Cleanup()
    {
        var socket = new BlockingSocket();
        var bundle = new ZLinkChannelRuntimeBundle(socket);
        var first = bundle.DisposeAsync().AsTask();
        await socket.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = bundle.DisposeAsync().AsTask();

        Assert.Same(first, second);
        Assert.False(second.IsCompleted);
        socket.Release.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, socket.DisposeCount);
    }

    [Fact]
    public async Task ChannelBundle_Dispose_Detaches_Manual_Generation_And_Joins_Connect()
    {
        var socket = new BlockingConnectableSocket();
        var bundle = new ZLinkChannelRuntimeBundle(socket);
        var connections = new ZLinkEndpointConnections();
        bundle.OwnManualConnectionAttachment(connections.Attach(
            endpoint => bundle.ConnectManual(socket, endpoint),
            endpoint => bundle.DisconnectManual(socket, endpoint)));
        var connect = Task.Run(
            () => connections.Connect("tcp://127.0.0.1:7401"));
        await socket.ConnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = bundle.DisposeAsync().AsTask();
        Assert.False(dispose.IsCompleted);

        socket.ConnectRelease.TrySetResult();
        await Task.WhenAll(connect, dispose).WaitAsync(TimeSpan.FromSeconds(5));
        connections.Connect("tcp://127.0.0.1:7402");

        Assert.Equal(1, socket.ConnectCount);
        Assert.Equal(1, socket.DisposeCount);
    }

    [Fact]
    public async Task HandlerOwner_Concurrent_Dispose_Callers_Share_Fallback_Cleanup()
    {
        await using var services = new ServiceCollection().BuildServiceProvider();
        var owner = new ZLinkScopedHandlerInstanceOwner(services);
        var handler = (BlockingHandler)owner.Resolve(typeof(BlockingHandler));
        var first = owner.DisposeAsync().AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = owner.DisposeAsync().AsTask();

        Assert.Same(first, second);
        Assert.False(second.IsCompleted);
        handler.Release.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, handler.DisposeCount);
    }

    [Fact]
    public async Task SpotPublisherRegistry_Concurrent_Create_And_Dispose_Closes_Admission_And_Disposes_All_Accepted_Bundles()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, SpotNodeProxy>();
        var proxy = (SpotNodeProxy)(object)node;
        var registry = new ZLinkSpotNodeBundleRegistry(
            new ZLinkFrameworkRegistration(),
            node,
            CancellationToken.None);

        var operations = Enumerable.Range(0, 100)
            .Select(index => Task.Run(() =>
            {
                try
                {
                    registry.GetOrCreatePublisherBundle($"channel-{index}");
                }
                catch (ObjectDisposedException)
                {
                }
            }))
            .Append(Task.Run(async () => await registry.DisposeAsync()))
            .ToArray();

        await Task.WhenAll(operations).WaitAsync(TimeSpan.FromSeconds(5));
        var first = registry.DisposeAsync().AsTask();
        var second = registry.DisposeAsync().AsTask();

        Assert.Same(first, second);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(proxy.CreatedSpots.Count, proxy.CreatedSpots.Sum(static spot => spot.DisposeCount));
        Assert.Throws<ObjectDisposedException>(() => registry.GetOrCreatePublisherBundle("after-close"));
    }

    [Fact]
    public async Task FrameworkRuntimeState_Concurrent_Dispose_Callers_Share_Blocked_Context_Cleanup()
    {
        await using var services = new ServiceCollection().BuildServiceProvider();
        var context = DispatchProxy.Create<IZLinkBackendContext, BlockingContextProxy>();
        var proxy = (BlockingContextProxy)(object)context;
        var failure = new InvalidOperationException("context cleanup failed");
        proxy.DisposeFailure = failure;
        var state = new ZLinkFrameworkComponentState(
            context,
            new ZLinkFrameworkRegistration(),
            services,
            new ZLinkRuntimeErrorSink(),
            new object());

        var first = state.DisposeAsync().AsTask();
        await proxy.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = state.DisposeAsync().AsTask();

        Assert.Same(first, second);
        Assert.False(second.IsCompleted);
        proxy.Release.TrySetResult();
        var firstFailure = await Assert.ThrowsAsync<InvalidOperationException>(
            () => first.WaitAsync(TimeSpan.FromSeconds(5)));
        var secondFailure = await Assert.ThrowsAsync<InvalidOperationException>(
            () => second.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Same(failure, firstFailure);
        Assert.Same(firstFailure, secondFailure);
        Assert.Equal(1, proxy.DisposeCount);
    }

    [Fact]
    public async Task StreamSessionSerialExecutor_Concurrent_Dispose_Callers_Share_Queued_Cleanup()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var executor = new ZLinkStreamSessionSerialExecutor(new object(), errors);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(executor.EnqueueInfrastructure(async () =>
        {
            entered.TrySetResult();
            await release.Task.ConfigureAwait(false);
        }));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var first = executor.DisposeAsync().AsTask();
        var second = executor.DisposeAsync().AsTask();
        Assert.Same(first, second);
        Assert.False(second.IsCompleted);

        release.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    private interface ITrackedLocationStore :
        Zlink.Framework.LocationProvider.IZLinkLocationStore,
        IAsyncDisposable;

    private interface ITrackedRelocationStore :
        Zlink.Framework.LocationProvider.IZLinkRelocationStore,
        IAsyncDisposable;

    private interface ITrackedProviderStore :
        Zlink.Framework.LocationProvider.IZLinkLocationStore,
        Zlink.Framework.LocationProvider.IZLinkRelocationStore,
        IAsyncDisposable;

    private class TrackedLocationStoreProxy : DispatchProxy
    {
        public int DisposeCount { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            Assert.NotNull(targetMethod);
            if (targetMethod.DeclaringType == typeof(IAsyncDisposable))
            {
                DisposeCount++;
                return ValueTask.CompletedTask;
            }

            throw new NotSupportedException(
                "This disposal probe does not execute Store operations.");
        }
    }

    private class TrackedRelocationStoreProxy : DispatchProxy
    {
        public int DisposeCount { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            Assert.NotNull(targetMethod);
            if (targetMethod.DeclaringType == typeof(IAsyncDisposable))
            {
                DisposeCount++;
                return ValueTask.CompletedTask;
            }

            throw new NotSupportedException(
                "This disposal probe does not execute Store operations.");
        }
    }

    private class TrackedProviderStoreProxy : DispatchProxy
    {
        public int DisposeCount { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            Assert.NotNull(targetMethod);
            if (targetMethod.DeclaringType == typeof(IAsyncDisposable))
            {
                DisposeCount++;
                return ValueTask.CompletedTask;
            }

            throw new NotSupportedException(
                "This disposal probe does not execute Store operations.");
        }
    }

    private sealed class StoreUsingDisposable(TrackedLocationStoreProxy store) : IAsyncDisposable
    {
        public int DisposeCount { get; private set; }

        public bool ObservedUsableStore { get; private set; }

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            ObservedUsableStore = store.DisposeCount == 0;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class BlockingSocket : IZLinkBackendSocket
    {
        private int _disposeCount;
        internal TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int DisposeCount => Volatile.Read(ref _disposeCount);
        public void Bind(string endpoint) { }
        public void SetChannelName(string channelName) { }
        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            Started.TrySetResult();
            await Release.Task.ConfigureAwait(false);
        }
    }

    private sealed class BlockingConnectableSocket : IZLinkBackendConnectableSocket
    {
        private int _connectCount;
        private int _disposeCount;

        internal TaskCompletionSource ConnectStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal TaskCompletionSource ConnectRelease { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal int ConnectCount => Volatile.Read(ref _connectCount);

        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void Bind(string endpoint) { }

        public void SetChannelName(string channelName) { }

        public void Connect(string endpoint)
        {
            Interlocked.Increment(ref _connectCount);
            ConnectStarted.TrySetResult();
            ConnectRelease.Task.GetAwaiter().GetResult();
        }

        public void Disconnect(string endpoint) { }

        public ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class EmptyBackendAdapterFactory : IZLinkBackendAdapterFactory
    {
        public IZLinkChannelBackendAdapter CreateChannelAdapter() => null!;
        public IZLinkSpotBackendAdapter CreateSpotAdapter() => null!;
        public IZLinkStreamBackendAdapter CreateStreamAdapter() => null!;
        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() => new EmptyMonitoringAdapter();
    }

    private class SpotNodeProxy : DispatchProxy
    {
        internal List<SpotProxy> CreatedSpots { get; } = [];

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name == nameof(IZLinkBackendSpotNode.CreateSpot))
            {
                var spot = DispatchProxy.Create<IZLinkBackendSpot, SpotProxy>();
                CreatedSpots.Add((SpotProxy)(object)spot);
                return spot;
            }

            throw new NotSupportedException(targetMethod.Name);
        }
    }

    private class BlockingContextProxy : DispatchProxy
    {
        private int _disposeCount;
        internal TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int DisposeCount => Volatile.Read(ref _disposeCount);
        internal Exception? DisposeFailure { get; set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendContext.Shutdown) => null,
                nameof(IAsyncDisposable.DisposeAsync) => Dispose(),
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private async ValueTask Dispose()
        {
            Interlocked.Increment(ref _disposeCount);
            Started.TrySetResult();
            await Release.Task.ConfigureAwait(false);
            if (DisposeFailure is not null)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(DisposeFailure).Throw();
        }
    }

    private class SpotProxy : DispatchProxy
    {
        private int _disposeCount;
        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendSpot.OnSendReady) => null,
                nameof(IAsyncDisposable.DisposeAsync) => Dispose(),
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private ValueTask Dispose()
        {
            Interlocked.Increment(ref _disposeCount);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class EmptyMonitoringAdapter : IZLinkMonitoringBackendAdapter
    {
        public IZLinkBackendSocketMonitor OpenSocketMonitor(IZLinkBackendSocket socket) => null!;
    }

    public sealed class BlockingHandler : IAsyncDisposable
    {
        private int _disposeCount;
        internal TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int DisposeCount => Volatile.Read(ref _disposeCount);
        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            Started.TrySetResult();
            await Release.Task.ConfigureAwait(false);
        }
    }

}
