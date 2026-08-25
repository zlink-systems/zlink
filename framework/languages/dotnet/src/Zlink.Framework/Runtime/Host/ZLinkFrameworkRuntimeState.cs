using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkFrameworkComponentState : IAsyncDisposable
{
    private readonly object _disposeGate = new();
    private readonly IDisposable _pressureMetricRegistration;
    private Task? _disposeTask;
    private int _operationFenced;

    public ZLinkFrameworkComponentState(
        IZLinkBackendRuntimeContext context,
        ZLinkFrameworkRegistration registration,
        IServiceProvider services,
        ZLinkRuntimeErrorSink errorSink,
        object executionOwner,
        ZLinkApplicationJobQueueCapacity applicationJobQueueCapacity)
    {
        Context = context;
        Registration = registration;
        ErrorSink = errorSink;
        ApplicationJobQueue = new ZLinkApplicationJobQueue(
            applicationJobQueueCapacity,
            receiveFlowFailureReporter: exception =>
                errorSink.ReportRuntimeTaskException(
                    "application-job-queue-receive-flow",
                    exception));
        context.ConfigureApplicationJobQueue(ApplicationJobQueue);
        Capacity = new ZLinkHostCapacityProjection(
            context,
            registration.InboundDispatchOptions,
            ApplicationJobQueue);
        TimerScheduler = new ZLinkTimerScheduler();
        TaskRunner = new ZLinkRuntimeTaskRunner(
            ErrorSink,
            StopTokenSource.Token,
            executionOwner,
            ownsSupervisor: true);
        _pressureMetricRegistration =
            ZLinkRuntimeMetrics.RegisterApplicationJobQueuePressure(
                ApplicationJobQueue.GetPressureMetrics);
    }

    public IZLinkBackendRuntimeContext Context { get; }

    public ZLinkFrameworkRegistration Registration { get; }

    internal ZLinkApplicationJobQueue ApplicationJobQueue { get; }

    internal ZLinkHostCapacityProjection Capacity { get; }

    public object SyncRoot { get; } = new();

    public CancellationTokenSource StopTokenSource { get; } = new();

    internal CancellationTokenSource ForceStopTokenSource { get; } = new();

    public ZLinkTimerScheduler TimerScheduler { get; }

    public ZLinkRuntimeTaskRunner TaskRunner { get; }

    public ZLinkRuntimeErrorSink ErrorSink { get; }

    public bool IsOperationFenced =>
        Volatile.Read(ref _operationFenced) != 0;

    public void FenceOperations() =>
        Interlocked.Exchange(ref _operationFenced, 1);

    public Dictionary<ZLinkChannelName, ZLinkChannelRuntimeBundle> SubscriberBundles { get; } = [];

    public Dictionary<ZLinkChannelName, ZLinkChannelRuntimeBundle> PublisherBundles { get; } = [];

    public Dictionary<ZLinkChannelName, ZLinkAutomaticFanoutSubscriberRuntime>
        AutomaticFanoutSubscriberRuntimes { get; } =
        [];

    public Dictionary<ZLinkChannelName, ZLinkChannelRuntimeBundle> ClientServerClientBundles { get; } = [];

    public Dictionary<ZLinkChannelName, ZLinkClientServerClientRuntime>
        ClientServerClientRuntimes { get; } = [];

    public Dictionary<ZLinkChannelName, ZLinkChannelRuntimeBundle> ClientServerServerBundles { get; } = [];

    public Dictionary<ZLinkSpotNodeName, ZLinkSpotNodeRuntime> SpotNodes { get; } = [];

    // Channel routing is fixed after startup. Keeping the resolved runtime in
    // the state avoids scanning every SpotNode and taking SyncRoot on each
    // application send.
    public Dictionary<ZLinkChannelName, ZLinkSpotNodeRuntime> RouteMeshNodesByChannel { get; } = [];

    public Dictionary<ZLinkStreamNodeName, ZLinkStreamNodeRuntime> StreamNodes { get; } = [];

    internal void BuildRouteMeshChannelIndex()
    {
        foreach (var registration in Registration.SpotNodes.Values)
        {
            if (!SpotNodes.TryGetValue(registration.SpotNodeName, out var nodeRuntime))
                continue;

            foreach (var membership in registration.ChannelMemberships)
            {
                if (!RouteMeshNodesByChannel.TryGetValue(
                        membership.ChannelName,
                        out var existing))
                {
                    RouteMeshNodesByChannel.Add(
                        membership.ChannelName,
                        nodeRuntime);
                    continue;
                }

                if (!ReferenceEquals(existing, nodeRuntime))
                    throw new ZLinkConfigurationException(
                        $"ChannelName '{membership.ChannelName}' is registered by "
                        + $"more than one process-local RouteMesh "
                        + $"('{existing.Registration.SpotNodeName}' and "
                        + $"'{nodeRuntime.Registration.SpotNodeName}').");
            }
        }
    }

    public List<Task> ListenerTasks { get; } = [];

    public bool TryGetSpotNodeByRoutingId(
        RoutingId nodeRid,
        out ZLinkSpotNodeRuntime nodeRuntime)
    {
        foreach (var candidate in SpotNodes.Values)
            if (candidate.Node.RoutingId == nodeRid)
            {
                nodeRuntime = candidate;
                return true;
            }

        nodeRuntime = null!;
        return false;
    }

    public void CancelActiveSpotOperations()
    {
        foreach (var node in SpotNodes.Values) node.CancelActiveOperations();
    }

    public void ForceStopStreamSessions()
    {
        ZLinkStreamNodeRuntime[] streams;
        lock (SyncRoot) streams = StreamNodes.Values.ToArray();
        foreach (var stream in streams) stream.ForceStopSessions();
    }

    public ValueTask DisposeAsync()
    {
        return BeginDispose(CancellationToken.None);
    }

    internal ValueTask ForceStopAsync(CancellationToken cancellationToken)
    {
        return BeginDispose(cancellationToken);
    }

    private ValueTask BeginDispose(CancellationToken forceStopToken)
    {
        lock (_disposeGate)
        {
            if (_disposeTask is not null) return new ValueTask(_disposeTask);

            RuntimeResources resources;
            lock (SyncRoot)
            {
                resources = new RuntimeResources(
                    SpotNodes.Values.ToArray(),
                    StreamNodes.Values.ToArray(),
                    ClientServerClientBundles.Values.ToArray(),
                    ClientServerClientRuntimes.Values.ToArray(),
                    ClientServerServerBundles.Values.ToArray(),
                    PublisherBundles.Values.ToArray(),
                    AutomaticFanoutSubscriberRuntimes.Values.ToArray(),
                    SubscriberBundles.Values.ToArray(),
                    ListenerTasks.ToArray());
            }

            return new ValueTask(
                _disposeTask = DisposeCoreAsync(resources, forceStopToken));
        }
    }

    private async Task DisposeCoreAsync(
        RuntimeResources resources,
        CancellationToken forceStopToken)
    {
        var failures = new List<Exception>();
        if (!forceStopToken.CanBeCanceled)
        {
            foreach (var node in resources.SpotNodes)
                await CaptureAsync(node.CloseLifecycleAsync)
                    .ConfigureAwait(false);
        }

        if (forceStopToken.CanBeCanceled)
            Capture(ForceStopTokenSource.Cancel);
        Capture(StopTokenSource.Cancel);
        Capture(ApplicationJobQueue.Dispose);
        foreach (var node in resources.SpotNodes) Capture(node.RequestStop);
        foreach (var stream in resources.StreamNodes) Capture(stream.RequestStop);

        await CaptureAsync(TaskRunner.StopAsync).ConfigureAwait(false);

        foreach (var stream in resources.StreamNodes)
            await CaptureAsync(() => DisposeSafelyAsync(stream)).ConfigureAwait(false);

        foreach (var bundle in resources.ClientServerClientBundles)
            await CaptureAsync(() => DisposeSafelyAsync(bundle)).ConfigureAwait(false);

        foreach (var runtime in resources.ClientServerClientRuntimes)
            await CaptureAsync(() => DisposeSafelyAsync(runtime)).ConfigureAwait(false);

        foreach (var bundle in resources.ClientServerServerBundles)
            await CaptureAsync(() => DisposeSafelyAsync(bundle)).ConfigureAwait(false);

        // A STREAM node's native session service is created from the shared
        // MeshNode. Destroy the dependent session service and socket before
        // destroying the MeshNode that owns their routing plane.
        foreach (var node in resources.SpotNodes)
            await CaptureAsync(() => DisposeSpotNodeSafelyAsync(
                    node,
                    forceStopToken))
                .ConfigureAwait(false);

        foreach (var bundle in resources.PublisherBundles)
            await CaptureAsync(() => DisposeSafelyAsync(bundle)).ConfigureAwait(false);

        foreach (var runtime in resources.AutomaticFanoutSubscriberRuntimes)
            await CaptureAsync(() => DisposeSafelyAsync(runtime)).ConfigureAwait(false);

        foreach (var bundle in resources.SubscriberBundles)
            await CaptureAsync(() => DisposeSafelyAsync(bundle)).ConfigureAwait(false);

        await CaptureAsync(() => WaitForListenerTasksAsync(resources.ListenerTasks)).ConfigureAwait(false);

        await CaptureAsync(TimerScheduler.DisposeAsync).ConfigureAwait(false);
        Capture(_pressureMetricRegistration.Dispose);
        Capture(ErrorSink.Dispose);
        Capture(ForceStopTokenSource.Dispose);
        Capture(StopTokenSource.Dispose);
        await CaptureAsync(() => DisposeSafelyAsync(Context)).ConfigureAwait(false);

        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
        return;

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    private static async ValueTask WaitForListenerTasksAsync(Task[] listenerTasks)
    {
        if (listenerTasks.Length == 0) return;

        try
        {
            await Task.WhenAll(listenerTasks);
        }
        catch (OperationCanceledException)
        {
        }
        catch (ObjectDisposedException)
        {
        }
        catch (ZlinkCloseException)
        {
        }
    }

    private sealed record RuntimeResources(
        ZLinkSpotNodeRuntime[] SpotNodes,
        ZLinkStreamNodeRuntime[] StreamNodes,
        ZLinkChannelRuntimeBundle[] ClientServerClientBundles,
        ZLinkClientServerClientRuntime[] ClientServerClientRuntimes,
        ZLinkChannelRuntimeBundle[] ClientServerServerBundles,
        ZLinkChannelRuntimeBundle[] PublisherBundles,
        ZLinkAutomaticFanoutSubscriberRuntime[] AutomaticFanoutSubscriberRuntimes,
        ZLinkChannelRuntimeBundle[] SubscriberBundles,
        Task[] ListenerTasks);

    private static async ValueTask DisposeSafelyAsync(IAsyncDisposable disposable)
    {
        try
        {
            await disposable.DisposeAsync();
        }
        catch (ObjectDisposedException)
        {
        }
        catch (ZlinkCloseException)
        {
        }
    }

    private static async ValueTask DisposeSpotNodeSafelyAsync(
        ZLinkSpotNodeRuntime node,
        CancellationToken forceStopToken)
    {
        try
        {
            if (forceStopToken.CanBeCanceled)
                await node.ForceStopAsync(forceStopToken).ConfigureAwait(false);
            else
                await node.DisposeAsync().ConfigureAwait(false);
        }
        catch (ObjectDisposedException)
        {
        }
        catch (ZlinkCloseException)
        {
        }
    }
}
