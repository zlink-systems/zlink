using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Backend.DotNet.Wrappers;

// RouteMesh 10.0.0 STREAM seam. Raw framed I/O stays on IStreamSocket; the
// bound-actor plane (bind/unbind/relay) moved onto IStreamSessionService, created
// from the owning MeshNode. Actor identity is resolved to a full ActorRef via the
// session bindings table so the framework seam can keep its actor-id-keyed shape.
internal sealed class ZLinkBackendStreamSocketWrapper : IZLinkBackendStreamSocket
{
    private readonly IStreamSocket _socket;
    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable _completions;
    private readonly ZLinkMeshDispatchPump? _ownedCompletionPump;
    private readonly bool _ownsNode;
    private readonly object _sendGate = new();
    private readonly object _sessionGate = new();
    private IStreamSessionService? _session;
    private bool _sessionStarted;

    // The shared Framework MeshNode supplies its completion table and owns its
    // pump. A standalone StreamNode supplies a private table and pump owned by
    // this wrapper, so bind/unbind use the same terminal-completion contract.
    public ZLinkBackendStreamSocketWrapper(
        IStreamSocket socket,
        IMeshNode node,
        ZLinkMeshCompletionTable completions,
        bool ownsNode,
        ZLinkMeshDispatchPump? ownedCompletionPump = null)
    {
        _socket = socket;
        _node = node;
        _completions = completions
            ?? throw new ArgumentNullException(nameof(completions));
        _ownsNode = ownsNode;
        _ownedCompletionPump = ownedCompletionPump;
    }

    internal IStreamSocket NativeSocket => _socket;

    public IZLinkBackendSocketPoller CreateReceivePoller() =>
        ZLinkBackendSocketPoller.Create(_socket);

    public void ApplySocketConfig(IZLinkSocketConfig config)
    {
        ZLinkBackendSocketOptionsMapper.Apply(_socket.Options, config);
        // Core uses -1 as the explicit unlimited value. The Framework's
        // StreamNode value 0 must not leave a binding-specific default in use.
        if (config.MaxMessageSize == 0)
            _socket.Options.MaxMessageSize = -1;
    }

    public string GetLastEndpoint() => _socket.Options.LastEndpoint;

    private IStreamSessionService Session()
    {
        if (_session is { } existing) return existing;
        lock (_sessionGate)
        {
            if (_session is null)
            {
                _session = _node.CreateStreamSessionService(_socket);
                _session.Start();
                _sessionStarted = true;
            }

            return _session;
        }
    }

    public void Bind(string endpoint)
    {
        _socket.Bind(endpoint);
    }

    public void SetTlsServer(string certPath, string keyPath, bool requireClientCert)
    {
        _socket.SetTlsServer(certPath, keyPath, requireClientCert);
    }

    public bool RecvPart(
        out RoutingId? sourceRoutingId,
        out Message? part,
        out bool hasMore,
        RecvFlags flags = RecvFlags.None) =>
        _socket.RecvPart(out sourceRoutingId, out part, out hasMore, flags);

    public bool RecvRetained(
        out ZLinkBackendStreamReceive? received,
        RecvFlags flags = RecvFlags.None)
    {
        var envelope = Received.Create();
        try
        {
            if (!_socket.RecvRetained(envelope, flags))
            {
                envelope.Dispose();
                received = null;
                return false;
            }

            received = new ZLinkBackendStreamReceive(
                envelope.RoutingId,
                envelope.Parts,
                hasMore: false,
                envelope);
            return true;
        }
        catch
        {
            envelope.Dispose();
            throw;
        }
    }

    public void OnSendReady(Action handler)
    {
        _socket.OnSendReady(handler);
    }

    public bool Send(RoutingId routingId, Message payload, SendFlags flags)
    {
        lock (_sendGate)
            return _socket.TrySend(routingId).Message(payload).Flags(flags).Submit();
    }

    public async ValueTask SendAsync(
        RoutingId routingId,
        Message payload,
        CancellationToken cancellationToken)
    {
        await _socket.Send(routingId)
            .Message(payload)
            .Async(cancellationToken)
            .ConfigureAwait(false);
    }

    public bool Send(RoutingId routingId, IReadOnlyList<Message> parts, SendFlags flags)
    {
        lock (_sendGate)
            return _socket.TrySend(routingId).Messages(parts).Flags(flags).Submit();
    }

    public void DisconnectPeer(RoutingId routingId)
    {
        _socket.DisconnectRid(routingId);
    }

    public async ValueTask BindActorAsync(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        // Core marks a session live from its connected observer event, which is
        // asynchronous to packet delivery — a bind triggered by the session's
        // first packet can outrun it and see NotConnected. Retry within the
        // bind timeout; the liveness event is milliseconds behind the packet.
        var deadline = DateTime.UtcNow
                       + (timeout > TimeSpan.Zero ? timeout : TimeSpan.FromSeconds(5));
        while (true)
        {
            var submit = await SubmitAndAwaitOperationAsync(
                    id => Session().BindActor(
                        sessionRid,
                        ToNativeActor(actor),
                        id,
                        timeout),
                    cancellationToken)
                .ConfigureAwait(false);
            if (submit != SubmitResult.NotConnected || DateTime.UtcNow >= deadline)
            {
                ThrowIfSubmitFailed(submit);
                return;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private ActorRef ToNativeActor(ZLinkBackendActorRef actor)
    {
        if (_node is not ZLinkManagedMeshNode managed)
            throw new InvalidOperationException(
                "Actor binding requires the Framework managed MeshNode.");
        return actor.ToNative(managed.MeshName);
    }

    public ValueTask UnbindActorAsync(
        RoutingId sessionRid,
        string actorId,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var session = Session();
        foreach (var binding in session.Bindings(sessionRid))
            if (string.Equals(binding.Actor.ActorId, actorId, StringComparison.Ordinal))
            {
                return UnbindAndAwaitAsync(
                    session,
                    sessionRid,
                    binding,
                    timeout,
                    cancellationToken);
            }

        return ValueTask.CompletedTask;
    }

    // Awaits a MeshNode operation (StreamBind/StreamUnbind) to terminal
    // completion so bind/unbind are observably complete (spec 31 §7 — binding
    // update runs on the infrastructure claim).
    private async ValueTask UnbindAndAwaitAsync(
        IStreamSessionService session,
        RoutingId sessionRid,
        StreamSessionBinding binding,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var submit = await SubmitAndAwaitOperationAsync(
                id => session.UnbindActor(
                    sessionRid,
                    binding.Actor,
                    binding.BindingGeneration,
                    id,
                    timeout),
                cancellationToken)
            .ConfigureAwait(false);
        ThrowIfSubmitFailed(submit);
    }

    private async ValueTask<SubmitResult> SubmitAndAwaitOperationAsync(
        Func<MeshOperationId, SubmitResult> submitOperation,
        CancellationToken cancellationToken)
    {
        var correlationId = _node.AllocateOperationId();
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var submit = _completions.RegisterBeforeSubmit(
            correlationId,
            (record, parts) =>
            {
                ZLinkMessageParts.DisposeAll(parts);
                var result = ZLinkMeshCompletionTable.MapResult(
                    record.TerminalResult, record.FailureErrno);
                if (result == RequestResult.Ok)
                    completion.TrySetResult();
                else
                    completion.TrySetException(
                        new ZlinkRequestException(
                            (ZlinkRequestException.ErrorCode)(int)result));
            },
            submitOperation);
        if (submit != SubmitResult.Ok)
            return submit;
        await using (_completions.RegisterCancellation(
                         correlationId,
                         cancellationToken,
                         () => completion.TrySetCanceled(cancellationToken))
                     .ConfigureAwait(false))
            await completion.Task.ConfigureAwait(false);
        return SubmitResult.Ok;
    }

    private static void ThrowIfSubmitFailed(SubmitResult submit)
    {
        if (submit != SubmitResult.Ok)
            throw new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)submit);
    }

    public bool SendBoundActor(
        RoutingId sessionRid,
        string actorId,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        var session = Session();
        foreach (var binding in session.Bindings(sessionRid))
            if (string.Equals(binding.Actor.ActorId, actorId, StringComparison.Ordinal))
                lock (_sendGate)
                    return session.SendToActor(sessionRid, binding.Actor, parts, flags)
                        == SubmitResult.Ok;

        return false;
    }

    public async ValueTask DisposeAsync()
    {
        IStreamSessionService? session;
        lock (_sessionGate)
        {
            session = _sessionStarted ? _session : null;
            _session = null;
        }

        if (session is not null)
            await session.DisposeAsync().ConfigureAwait(false);
        await _socket.DisposeAsync().ConfigureAwait(false);

        // The shared framework MeshNode is owned by its spot node runtime; only a
        // standalone minted node is disposed here.
        if (_ownsNode)
        {
            if (_ownedCompletionPump is not null)
                await _ownedCompletionPump.DisposeAsync().ConfigureAwait(false);
            await _node.DisposeAsync().ConfigureAwait(false);
        }
    }
}
