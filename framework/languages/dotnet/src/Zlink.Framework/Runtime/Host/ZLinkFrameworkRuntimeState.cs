using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Host;

internal sealed class ZLinkFrameworkComponentState : IAsyncDisposable
{
    private const int DefaultPendingRequestLimit = 65_536;
    private const int DefaultCompletionSendLimit = 65_536;
    private readonly object _disposeGate = new();
    private Task? _disposeTask;
    private int _operationFenced;

    public ZLinkFrameworkComponentState(
        IZLinkBackendContext context,
        ZLinkFrameworkRegistration registration,
        IServiceProvider services,
        ZLinkRuntimeErrorSink errorSink,
        object executionOwner)
    {
        Context = context;
        Registration = registration;
        ErrorSink = errorSink;
        InboundDispatchBudget = new ZLinkInboundDispatchBudget(
            registration.InboundDispatchOptions.EffectiveApplicationHwmBytes,
            registration.ResolveMaximumApplicationMessageBytes());
        CompletionAdmission = new ZLinkCompletionAdmissionOwner(
            DefaultPendingRequestLimit,
            DefaultCompletionSendLimit,
            registration.InboundDispatchOptions.EffectiveApplicationHwmBytes);
        TimerScheduler = new ZLinkTimerScheduler();
        TaskRunner = new ZLinkRuntimeTaskRunner(
            ErrorSink,
            StopTokenSource.Token,
            executionOwner,
            ownsSupervisor: true);
    }

    public IZLinkBackendContext Context { get; }

    public ZLinkFrameworkRegistration Registration { get; }

    public object SyncRoot { get; } = new();

    public CancellationTokenSource StopTokenSource { get; } = new();

    public ZLinkInboundDispatchBudget InboundDispatchBudget { get; }

    public ZLinkCompletionAdmissionOwner CompletionAdmission { get; }

    public ZLinkTimerScheduler TimerScheduler { get; }

    public ZLinkRuntimeTaskRunner TaskRunner { get; }

    public ZLinkRuntimeErrorSink ErrorSink { get; }

    public bool IsOperationFenced =>
        Volatile.Read(ref _operationFenced) != 0;

    public void FenceOperations() =>
        Interlocked.Exchange(ref _operationFenced, 1);

    public Dictionary<string, ZLinkChannelRuntimeBundle> SubscriberBundles { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkChannelRuntimeBundle> PublisherBundles { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkAutomaticFanoutSubscriberRuntime>
        AutomaticFanoutSubscriberRuntimes { get; } =
        new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkChannelRuntimeBundle> ClientServerClientBundles { get; } =
        new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkClientServerClientRuntime>
        ClientServerClientRuntimes { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkChannelRuntimeBundle> ClientServerServerBundles { get; } =
        new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkSpotNodeRuntime> SpotNodes { get; } = new(StringComparer.Ordinal);

    public Dictionary<string, ZLinkStreamNodeRuntime> StreamNodes { get; } = new(StringComparer.Ordinal);

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

        Capture(StopTokenSource.Cancel);
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

        Capture(CompletionAdmission.Dispose);
        await CaptureAsync(TimerScheduler.DisposeAsync).ConfigureAwait(false);
        Capture(ErrorSink.Dispose);
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
