using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.Runtime.Spots;

internal sealed partial class ZLinkEntrySpotActivation :
    IZLinkEntrySpotContext,
    IZLinkCurrentSpotActivation,
    IZLinkSpotHandlerRegistrySink,
    IAsyncDisposable
{
    ZLinkUserSpotExecutionMode IZLinkCurrentSpotActivation.ExecutionMode
        => ZLinkUserSpotExecutionMode.PerActor;
    private static readonly AsyncLocal<ZLinkEntrySpotActivation?> Current = new();
    private readonly ZLinkSpotActorHandlerRegistry _actorHandlers;
    private readonly SemaphoreSlim _actorJoinDrainGate = new(1, 1);
    private readonly ZLinkSpotActorJoinRegistry _actorJoins = new();
    private readonly ZLinkSpotActorMembership _actors = new();
    private ZLinkSpotActivationDispatcher _dispatcher = null!;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly ZLinkEntrySpotHandlerExecutor _handlerExecutor;
    private readonly ZLinkScopedHandlerInstanceOwner _handlerInstances;
    private readonly ZLinkSpotHandlerInvoker _invoker;
    private readonly IZLinkBackendSpot _nativeSpot;
    private ZLinkSpotOutboundTransport _outbound = null!;
    private ZLinkSpotOutboundEndpoint _outboundEndpoint = null!;
    private readonly ZLinkSpotPacketRegistry _packets = new();
    private readonly SemaphoreSlim _routeDrainGate = new(1, 1);
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly AsyncServiceScope _scope;
    private ZLinkSerialExecutionQueue _serial = null!;
    private readonly CancellationTokenSource _stopSource = new();
    private readonly ZLinkSpotSubscriptionRegistry _subscriptions = new();
    private readonly ZLinkSpotTimerRegistry _timers;
    private readonly object _lifecycleGate = new();
    private bool _configurationOpen = true;
    private Task? _finalization;
    private int _disposed;

    public ZLinkEntrySpotActivation(
        ZLinkFrameworkRuntime runtime,
        IServiceProvider services,
        AsyncServiceScope scope,
        IZLinkBackendSpot nativeSpot,
        string spotId,
        Type entrySpotType,
        RoutingId nodeRid,
        string spotNodeName,
        string channelName,
        TimeSpan defaultRequestTimeout,
        ZLinkSpotOutboundTransport outbound,
        ZLinkTimerScheduler? timerScheduler = null)
    {
        _runtime = runtime;
        _timers = new ZLinkSpotTimerRegistry(
            () => runtime.Flow.CaptureEnabled,
            scheduler: timerScheduler);
        _nativeSpot = nativeSpot;
        SpotId = ZLinkSpotId.Require(spotId, nameof(spotId));
        NodeRid = nodeRid;
        SpotNodeName = spotNodeName;
        ChannelName = channelName;
        DefaultRequestTimeout = defaultRequestTimeout;
        _outbound = outbound;
        _scope = scope;
        _handlerInstances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);
        try
        {
            EntrySpot = (IZLinkEntrySpot)ActivatorUtilities.CreateInstance(
                _scope.ServiceProvider,
                entrySpotType,
                this);
            if (!ReferenceEquals(EntrySpot.Context, this))
                throw new InvalidOperationException(
                    $"Entry SPOT '{entrySpotType.FullName}' must expose the context provided by the runtime.");

            _invoker = new ZLinkSpotHandlerInvoker(
                _handlerInstances,
                EntrySpot,
                SpotNodeName,
                _runtime.Registration.Codecs,
                _runtime.Registration.StreamCompressionCodec,
                actorHandlerInstances: ResolveActorHandlerInstances);
            _handlerExecutor = new ZLinkEntrySpotHandlerExecutor(
                _invoker);
            _actorHandlers = new ZLinkSpotActorHandlerRegistry(
                ZLinkSpotActorHandlerSurface.EntrySpot,
                EntrySpot.GetType());
            Handlers = new ZLinkSpotHandlerRegistrySurface(this);
        }
        catch
        {
            _stopSource.Dispose();
            throw;
        }
    }

    public IZLinkEntrySpot EntrySpot { get; }

    private ZLinkScopedHandlerInstanceOwner ResolveActorHandlerInstances(
        IZLinkActor actor)
    {
        var state = _runtime.GetOrCreateActorState(actor.Context.ActorId);
        return state.HandlerInstances;
    }

    public IZLinkRuntimeFailureReporter ErrorSink => _runtime.ErrorSink;

    public string SpotNodeName { get; }

    private bool IsDisposed => Volatile.Read(ref _disposed) != 0;

    internal void RequestStop()
    {
        _serial?.Complete();
        _stopSource.Cancel();
    }

    internal void InitializeRuntimeResources(
        ZLinkCompletionAdmissionOwner? completionAdmission = null)
    {
        _outboundEndpoint = new ZLinkSpotOutboundEndpoint(this, _outbound, _runtime);
        _dispatcher = new ZLinkSpotActivationDispatcher(
            _runtime,
            _nativeSpot,
            ChannelName,
            SpotId,
            _packets,
            _actorJoins,
            _actors,
            _subscriptions,
            () => _actorHandlers,
            () => _invoker,
            replySubmitter: _outbound.Submitter,
            completionAdmission: completionAdmission,
            acceptActorJoinWithoutHandler: true);
        var errorSink = _runtime.ErrorSink;
        _serial = new ZLinkSerialExecutionQueue(
            new ZLinkRuntimeTaskRunner(
                errorSink,
                _stopSource.Token,
                _runtime.ExecutionOwner),
            errorSink,
            _stopSource.Token);
    }

    public ValueTask DisposeAsync()
    {
        TaskCompletionSource completion;
        lock (_lifecycleGate)
        {
            if (_finalization is not null) return new ValueTask(_finalization);

            Volatile.Write(ref _disposed, 1);
            completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _finalization = completion.Task;
        }

        _ = CompleteFinalizationAsync(completion);
        return new ValueTask(completion.Task);
    }

    private async Task CompleteFinalizationAsync(TaskCompletionSource completion)
    {
        try
        {
            await FinalizeAsync().ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }

    private async Task FinalizeAsync()
    {
        var failures = new List<Exception>();
        Capture(RequestStop);
        await CaptureAsync(_timers.DisposeAsync).ConfigureAwait(false);
        if (_serial is not null) await CaptureAsync(_serial.DisposeAsync).ConfigureAwait(false);
        Capture(_stopSource.Dispose);
        await CaptureAsync(_handlerInstances.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_scope.DisposeAsync).ConfigureAwait(false);
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);

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

    public string ChannelName { get; }

    public string MeshName => ChannelName;

    public TimeSpan DefaultRequestTimeout { get; }

    public ZLinkCodecRegistryBuilder Codecs => _runtime.Registration.Codecs;

    ZLinkMessageFlowTracer IZLinkCurrentSpotActivation.Flow => _runtime.Flow;

    public IZLinkSpotHandlerRegistry Handlers { get; }

    public IZLinkSpotOutbound Outbound => _outboundEndpoint;

    ZLinkSpotOutboundEndpoint IZLinkCurrentSpotActivation.OutboundEndpoint => _outboundEndpoint;

    // Entry Spots are node-scoped and never cross an owner cutover.
    void IZLinkCurrentSpotActivation.EnsureOperationAllowed()
    {
    }

    public string SpotId { get; }

    public ulong ObjectGeneration => _nativeSpot.LifecycleGeneration;

    public RoutingId NodeRid { get; }

    public ValueTask DestroyActorAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(actor);
        cancellationToken.ThrowIfCancellationRequested();

        if (ZLinkBoundSessionDispatchScope.TryDefer(
                actor.Context.ActorId,
                ct => DestroyActorCoreAsync(actor, ct)))
            return ValueTask.CompletedTask;

        return DestroyActorCoreAsync(actor, cancellationToken);
    }

    private async ValueTask DestroyActorCoreAsync(
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        await _runtime.DestroyActorAsync(
                NodeRid,
                actor,
                cancellationToken)
            .ConfigureAwait(false);
        _actors.RemoveIfCurrent(actor);
    }

    public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
        Func<CancellationToken, TResult> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        return new ZLinkWorkerCall<TResult>(
            _runtime.WorkerPool,
            work,
            _runtime.ErrorSink);
    }

    public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
        Func<CancellationToken, ValueTask<TResult>> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        return new ZLinkIoWorkerCall<TResult>(
            _runtime.WorkerPool.ShutdownToken,
            work,
            _runtime.ErrorSink);
    }

    public void Configure()
    {
        EntrySpot.Configure();
        _configurationOpen = false;
        _packets.Bind(EntrySpot);
        _subscriptions.Bind(EntrySpot, _nativeSpot);
        _actorHandlers.Bind();
    }

    public ValueTask InitializeAsync(CancellationToken cancellationToken)
    {
        return ExecuteAsync(
            static async (activation, ct) =>
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    activation._runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                await activation.EntrySpot.OnInitializeAsync(ct).ConfigureAwait(false);
            },
            cancellationToken);
    }

    public async ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static async (activation, ct) =>
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    activation._runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                await ZLinkSpotClosingInvocation.InvokeAsync(
                        activation.EntrySpot.OnClosingAsync,
                        ZLinkSpotCloseReason.HostShutdown,
                        DateTimeOffset.UtcNow + activation.DefaultRequestTimeout)
                    .ConfigureAwait(false);
            },
            cancellationToken).ConfigureAwait(false);
    }

    public bool TryResolveActorPacket(
        Type actorType,
        ZlinkStreamHeader header,
        out ZLinkSpotActorPacketDescriptor? descriptor)
    {
        return _actorHandlers.TryResolve(actorType, header, out descriptor);
    }

    public bool TryResolvePacket(
        ZLinkEnvelopeHeader header,
        out ZLinkSpotDescriptor? descriptor)
    {
        return _packets.TryResolve(header, out descriptor);
    }

    public async ValueTask InvokePacketAsync(
        ZLinkSpotDescriptor descriptor,
        object? message,
        CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static (activation, state, ct) => activation._invoker.InvokePacketAsync(
                state.Descriptor,
                state.Message,
                ct),
            (Descriptor: descriptor, Message: message),
            cancellationToken).ConfigureAwait(false);
    }

    public bool TryResolveActorJoined(
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return _actorHandlers.TryResolveJoined(actorType, out descriptor);
    }

    public bool TryResolveActorCreated(
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return _actorHandlers.TryResolveCreated(actorType, out descriptor);
    }

    public bool TryResolveActorLeft(
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return _actorHandlers.TryResolveLeft(actorType, out descriptor);
    }

    public bool TryResolveActorDisconnected(
        Type actorType,
        out ZLinkSpotActorLifecycleDescriptor? descriptor)
    {
        return _actorHandlers.TryResolveDisconnected(actorType, out descriptor);
    }

    public async ValueTask InvokeActorPacketAsync(
        ZLinkSpotActorPacketDescriptor descriptor,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        await ExecuteActorPacketAsync(
            static (activation, state, ct) => activation._handlerExecutor.InvokeActorPacketAsync(
                state.Descriptor,
                state.Actor,
                state.Header,
                state.Body,
                ct),
            (Descriptor: descriptor, Actor: actor, Header: header, Body: body),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorReply> InvokeActorPacketForReplyAsync(
        ZLinkSpotActorPacketDescriptor descriptor,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body,
        CancellationToken cancellationToken)
    {
        var call = new ActorPacketReplyCallState(descriptor, actor, header, body);
        await ExecuteActorPacketAsync(
            static async (activation, state, ct) =>
            {
                state.Reply = await activation._handlerExecutor.InvokeActorPacketForReplyAsync(
                        state.Descriptor,
                        state.Actor,
                        state.Header,
                        state.Body,
                        ct)
                    .ConfigureAwait(false);
            },
            call,
            cancellationToken).ConfigureAwait(false);

        return call.Reply
               ?? throw new InvalidOperationException(
                   $"Entry Spot actor packet reply for '{descriptor.MessageName}' was null.");
    }

    public async ValueTask InvokeActorLifecycleAsync(
        ZLinkSpotActorLifecycleDescriptor descriptor,
        IZLinkActor actor,
        ZLinkMessage? request,
        CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static async (activation, state, ct) =>
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    activation._runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                await activation._invoker.InvokeActorLifecycleAsync(
                        state.Descriptor,
                        state.Actor,
                        state.Request,
                        ct)
                    .ConfigureAwait(false);
            },
            (Descriptor: descriptor, Actor: actor, Request: request),
            cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask<ZLinkActorCreateResponse> InvokeActorCreateAsync(
        ZLinkSpotActorLifecycleDescriptor descriptor,
        IZLinkActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var call = new ActorCreateCallState(descriptor, actor, request);
        await ExecuteAsync(
            static async (activation, state, ct) =>
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    activation._runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                state.Response = await activation._invoker.InvokeActorCreateAsync(
                        state.Descriptor,
                        state.Actor,
                        state.Request,
                        ct)
                    .ConfigureAwait(false);
            },
            call,
            cancellationToken).ConfigureAwait(false);
        return call.Response;
    }

    public async ValueTask InvokeActorDisconnectedAsync(
        ZLinkSpotActorLifecycleDescriptor descriptor,
        IZLinkActor actor,
        CancellationToken cancellationToken)
    {
        await ExecuteAsync(
            static async (activation, state, ct) =>
            {
                using var flow = ZLinkFlowContext.Enter(
                    null,
                    null,
                    activation._runtime.Flow.CaptureEnabled,
                    ZLinkFlowOrigin.Lifecycle);
                await activation._invoker.InvokeActorLifecycleAsync(
                        state.Descriptor,
                        state.Actor,
                        ct)
                    .ConfigureAwait(false);
            },
            (Descriptor: descriptor, Actor: actor),
            cancellationToken).ConfigureAwait(false);
    }

    private sealed class ActorPacketReplyCallState(
        ZLinkSpotActorPacketDescriptor descriptor,
        IZLinkActor actor,
        ZlinkStreamHeader header,
        Message body)
    {
        public ZLinkSpotActorPacketDescriptor Descriptor { get; } = descriptor;

        public IZLinkActor Actor { get; } = actor;

        public ZlinkStreamHeader Header { get; } = header;

        public Message Body { get; } = body;

        public ZLinkActorReply? Reply { get; set; }
    }

    private sealed class ActorCreateCallState(
        ZLinkSpotActorLifecycleDescriptor descriptor,
        IZLinkActor actor,
        ZLinkMessage request)
    {
        public ZLinkSpotActorLifecycleDescriptor Descriptor { get; } = descriptor;
        public IZLinkActor Actor { get; } = actor;
        public ZLinkMessage Request { get; } = request;
        public ZLinkActorCreateResponse Response { get; set; } =
            ZLinkActorCreateResponse.Accept();
    }

}
