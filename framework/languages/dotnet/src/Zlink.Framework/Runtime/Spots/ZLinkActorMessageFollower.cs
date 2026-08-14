namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkActorMessageFollower
{
    private const int DirectReplyCapacity = 4096;
    private readonly ZLinkFrameworkRuntime _runtime;
    private readonly System.Collections.Concurrent.ConcurrentDictionary<MessageFollowKey, ActorQueue>
        _queues = new();
    private readonly ZLinkDirectReplyCompletionRegistry<
        DirectReplyKey,
        PendingDirectReply> _directReplyCompletions = new(
            DirectReplyCapacity,
            ZLinkRelocationReplyLifetime.TerminalRetention);

    public ZLinkActorMessageFollower(ZLinkFrameworkRuntime runtime)
    {
        _runtime = runtime;
    }

    public void Enqueue(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        ZLinkBackendActorRouteContext routeContext,
        ZlinkStreamHeader header,
        Message body,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null,
        ReadOnlyMemory<byte> applicationMetadata = default)
    {
        _ = EnqueueTracked(
            messageFollowRoute,
            sourceNodeRid,
            sourceSessionRid,
            requestId,
            flags,
            routeContext,
            header,
            body,
            sourceNodeGeneration,
            requestSource,
            directReply,
            applicationMetadata);
    }

    internal Task<bool> EnqueueTracked(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        ZLinkBackendActorRouteContext routeContext,
        ZlinkStreamHeader header,
        Message body,
        ulong sourceNodeGeneration = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null,
        ReadOnlyMemory<byte> applicationMetadata = default)
    {
        _runtime.ShutdownToken.ThrowIfCancellationRequested();
        MessageFollowFrame? frame = null;
        DirectReplyKey? directReplyKey = null;
        PendingDirectReply? directReplyPending = null;
        try
        {
            if (directReply is not null
                && header.Kind == ZlinkStreamMessageKind.Request
                && string.IsNullOrEmpty(routeContext.ReplyCapability))
            {
                var capability = CreateReplyCapability(
                    messageFollowRoute.SourceActor.NodeRid,
                    routeContext.DeadlineUnixMs);
                var replyKey = new DirectReplyKey(
                    messageFollowRoute.SourceActor.ActorId,
                    requestId,
                    capability);
                var pending = new PendingDirectReply(
                    directReply,
                    routeContext.DeadlineUnixMs);
                if (!_directReplyCompletions.TryRegister(replyKey, pending))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor ref '{messageFollowRoute.SourceActor.ActorId}' could not preserve its Message Follow reply route.");
                directReplyKey = replyKey;
                directReplyPending = pending;
                routeContext = routeContext with
                {
                    ReplyRequestId = requestId,
                    ReplyFlags = flags,
                    ReplyCapability = capability
                };
                if (!_runtime.TryRunDetached(
                        "actor Message Follow reply expiry",
                        ct => ExpireDirectReplyAsync(
                            replyKey,
                            pending,
                            ct)))
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.ShuttingDown,
                        "The runtime stopped before the Message Follow reply route was registered.");
            }
            frame = new MessageFollowFrame(
                messageFollowRoute,
                sourceNodeRid,
                sourceSessionRid,
                requestId,
                flags,
                routeContext,
                sourceNodeGeneration,
                requestSource,
                applicationMetadata.ToArray(),
                header,
                ZLinkStreamProtocolDefaults.EncodeHeader(header).ToArray(),
                body.ToArray());
            var key = new MessageFollowKey(
                messageFollowRoute.SourceActor.NodeRid,
                messageFollowRoute.SourceActor.ActorId,
                messageFollowRoute.SourceActor.Generation,
                messageFollowRoute.TargetActor.NodeRid,
                messageFollowRoute.TargetActor.Generation,
                messageFollowRoute.SourceNodeGeneration,
                messageFollowRoute.TargetNodeGeneration,
                messageFollowRoute.SourceAuthorityOwnerGeneration,
                messageFollowRoute.TargetAuthorityOwnerGeneration,
                messageFollowRoute.SourceOwnerLeaseGeneration,
                messageFollowRoute.TargetOwnerLeaseGeneration);
            while (!_queues.GetOrAdd(
                       key,
                       static (queueKey, follower) => new ActorQueue(follower, queueKey),
                       this)
                   .TryEnqueue(frame))
            {
            }
            return frame.Completion;
        }
        catch
        {
            if (directReplyKey is { } key
                && directReplyPending is { } pending)
                _directReplyCompletions.TryRemove(
                    key,
                    pending,
                    rememberTerminal: false);
            throw;
        }
    }

    public async ValueTask<bool> TryCompleteDirectReplyAsync(
        string actorId,
        ulong requestId,
        uint flags,
        string replyCapability,
        RoutingId authenticatedResponder,
        RoutingId declaredResponder,
        byte[] frame,
        CancellationToken cancellationToken)
    {
        if (flags != ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind
            || authenticatedResponder.IsEmpty
            || authenticatedResponder != declaredResponder)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_follow_reply_rejected actor={actorId} request_id={requestId} "
                + $"flags={flags} authenticated={authenticatedResponder} "
                + $"declared={declaredResponder}");
            return false;
        }
        var key = new DirectReplyKey(
            actorId,
            requestId,
            replyCapability);
        if (!_directReplyCompletions.TryGet(key, out var pending))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_follow_reply_not_found actor={actorId} request_id={requestId}");
            return false;
        }
        if (!pending.TryBeginDelivery())
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_follow_reply_already_claimed actor={actorId} request_id={requestId}");
            return true;
        }
        var outcome = DirectReplyDeliveryResult.Interrupted;
        try
        {
            outcome = await SubmitDirectReplyOnceAsync(
                    pending,
                    [frame],
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (outcome is not DirectReplyDeliveryResult.Interrupted
                || pending.IsExpired)
            {
                pending.Complete();
                _directReplyCompletions.TryRemove(
                    key,
                    pending,
                    rememberTerminal: true);
            }
            else
            {
                pending.ReleaseDelivery();
            }
        }
        return outcome is not DirectReplyDeliveryResult.DeadlineExpired
               || pending.HasExplicitDeadline;
    }

    internal PreservedDirectReply PreserveDirectReply(
        RoutingId replyNodeRid,
        string actorId,
        ulong requestId,
        ulong deadlineUnixMs,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult> directReply)
    {
        if (replyNodeRid.IsEmpty
            || string.IsNullOrWhiteSpace(actorId)
            || requestId == 0)
            throw new ArgumentOutOfRangeException(nameof(requestId));
        ArgumentNullException.ThrowIfNull(directReply);

        var capability = CreateReplyCapability(replyNodeRid, deadlineUnixMs);
        var key = new DirectReplyKey(actorId, requestId, capability);
        var pending = new PendingDirectReply(directReply, deadlineUnixMs);
        if (!_directReplyCompletions.TryRegister(key, pending))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor ref '{actorId}' could not preserve its relocation reply route.");
        if (!_runtime.TryRunDetached(
                "actor relocation reply expiry",
                ct => ExpireDirectReplyAsync(
                    key,
                    pending,
                    ct)))
        {
            _directReplyCompletions.TryRemove(
                key,
                pending,
                rememberTerminal: false);
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "The runtime stopped before the relocation reply route was registered.");
        }

        return new PreservedDirectReply(
            capability,
            (parts, flags) => CompleteLocalDirectReply(
                key,
                pending,
                parts,
                flags));
    }

    internal bool TryResolveReplyRoute(
        string replyCapability,
        out RoutingId replyNodeRid,
        out ulong deadlineUnixMs)
    {
        replyNodeRid = default;
        deadlineUnixMs = 0;
        var components = replyCapability.Split('.');
        if (components[0] is not ("v1" or "v2")
            || (components[0] == "v1" && components.Length != 3)
            || (components[0] == "v2" && components.Length != 4))
            return false;
        if (components[0] == "v2"
            && (!ulong.TryParse(
                    components[2],
                    System.Globalization.NumberStyles.None,
                    System.Globalization.CultureInfo.InvariantCulture,
                    out deadlineUnixMs)
                || deadlineUnixMs > long.MaxValue))
            return false;
        try
        {
            replyNodeRid = RoutingId.FromHex(components[1]);
            return !replyNodeRid.IsEmpty;
        }
        catch (Exception exception)
            when (exception is ArgumentException or FormatException)
        {
            return false;
        }
    }

    internal async ValueTask<bool> TryCompleteLocalDirectReplyAsync(
        string actorId,
        ulong requestId,
        uint flags,
        string replyCapability,
        IReadOnlyList<Message> parts,
        CancellationToken cancellationToken)
    {
        if (flags != ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind)
            return false;
        var key = new DirectReplyKey(actorId, requestId, replyCapability);
        if (!_directReplyCompletions.TryGet(key, out var pending))
            return false;
        if (!pending.TryBeginDelivery())
        {
            ZLinkMessageParts.DisposeAll(parts);
            return true;
        }
        var frames = new byte[parts.Count][];
        for (var index = 0; index < parts.Count; index++)
            frames[index] = parts[index].ToArray();
        ZLinkMessageParts.DisposeAll(parts);
        var outcome = DirectReplyDeliveryResult.Interrupted;
        try
        {
            outcome = await SubmitDirectReplyOnceAsync(
                    pending,
                    frames,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (outcome is not DirectReplyDeliveryResult.Interrupted
                || pending.IsExpired)
            {
                pending.Complete();
                _directReplyCompletions.TryRemove(
                    key,
                    pending,
                    rememberTerminal: true);
            }
            else
            {
                pending.ReleaseDelivery();
            }
        }
        return true;
    }

    private SubmitResult CompleteLocalDirectReply(
        DirectReplyKey key,
        PendingDirectReply pending,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        if (!_directReplyCompletions.TryGet(key, out var current)
            || !ReferenceEquals(current, pending)
            || !pending.TryBeginDelivery())
            return SubmitResult.Terminated;
        var terminal = false;
        try
        {
            if (pending.IsExpired)
            {
                pending.Complete();
                terminal = true;
                return SubmitResult.Terminated;
            }
            terminal = true;
            return pending.Reply(parts, flags);
        }
        finally
        {
            pending.ReleaseDelivery();
            if (terminal)
                _directReplyCompletions.TryRemove(
                    key,
                    pending,
                    rememberTerminal: true);
        }
    }

    private static string CreateReplyCapability(
        RoutingId replyNodeRid,
        ulong deadlineUnixMs)
    {
        if (replyNodeRid.IsEmpty)
            throw new ArgumentOutOfRangeException(nameof(replyNodeRid));
        if (deadlineUnixMs > long.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(deadlineUnixMs));
        return string.Create(
            System.Globalization.CultureInfo.InvariantCulture,
            $"v2.{replyNodeRid.ToHex()}.{deadlineUnixMs}.{Guid.NewGuid():N}");
    }

    private async ValueTask ExpireDirectReplyAsync(
        DirectReplyKey key,
        PendingDirectReply pending,
        CancellationToken cancellationToken)
    {
        try
        {
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var deadline = ResolveDeadlineUnixMs(
                pending.DeadlineUnixMs,
                now,
                _runtime.Registration.DefaultRequestTimeout);
            var remaining = Math.Max(0, deadline - now);
            if (remaining > 0)
                await Task.Delay(
                        TimeSpan.FromMilliseconds(remaining),
                        cancellationToken)
                    .ConfigureAwait(false);
        }
        finally
        {
            pending.Expire();
            _directReplyCompletions.TryRemove(
                key,
                pending,
                rememberTerminal: true);
        }
    }

    private static ValueTask<DirectReplyDeliveryResult>
        SubmitDirectReplyOnceAsync(
        PendingDirectReply pending,
        IReadOnlyList<byte[]> frames,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (pending.IsExpired)
            return ValueTask.FromResult(
                DirectReplyDeliveryResult.DeadlineExpired);
        var messages = new Message[frames.Count];
        SubmitResult result;
        for (var index = 0; index < frames.Count; index++)
            messages[index] = Message.From(frames[index]);
        try
        {
            result = pending.Reply(messages, SendFlags.DontWait);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(messages);
        }
        return ValueTask.FromResult(result == SubmitResult.Ok
            ? DirectReplyDeliveryResult.Submitted
            : DirectReplyDeliveryResult.TerminalRejected);
    }

    private static long ResolveDeadlineUnixMs(
        ulong deadlineUnixMs,
        long now,
        TimeSpan fallbackTimeout) =>
        deadlineUnixMs is > 0 and <= long.MaxValue
            ? checked((long)deadlineUnixMs)
            : checked(now + (long)fallbackTimeout.TotalMilliseconds);

    internal static ZLinkBackendActorRouteContext AdvanceRoute(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        ZLinkBackendActorRouteContext routeContext,
        ulong requestId,
        uint flags)
    {
        if (routeContext.IsDirectRoute
            && routeContext.MessageFollowHopCount >= 8)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"Actor ref '{messageFollowRoute.SourceActor.ActorId}' cannot use Message Follow because the chain reached the 8-hop limit.");
        return routeContext.IsDirectRoute
            ? new ZLinkBackendActorRouteContext(
                routeContext.OperationId,
                checked((byte)(routeContext.MessageFollowHopCount + 1)),
                messageFollowRoute.TargetNodeGeneration,
                messageFollowRoute.TargetAuthorityOwnerGeneration,
                messageFollowRoute.TargetOwnerLeaseGeneration,
                requestId,
                flags,
                routeContext.ReplyCapability,
                routeContext.DeadlineUnixMs)
            : routeContext.IsBoundSessionRoute
                ? new ZLinkBackendActorRouteContext(
                    routeContext.OperationId,
                    0,
                    messageFollowRoute.TargetNodeGeneration,
                    messageFollowRoute.TargetAuthorityOwnerGeneration,
                    messageFollowRoute.TargetOwnerLeaseGeneration,
                    routeContext.ReplyRequestId != 0 ? requestId : 0,
                    routeContext.ReplyRequestId != 0 ? flags : 0,
                    routeContext.ReplyCapability,
                    routeContext.DeadlineUnixMs,
                    IsBoundSessionRoute: true)
            : routeContext.ReplyRequestId != 0
                ? new ZLinkBackendActorRouteContext(
                    default,
                    0,
                    messageFollowRoute.TargetNodeGeneration,
                    messageFollowRoute.TargetAuthorityOwnerGeneration,
                    messageFollowRoute.TargetOwnerLeaseGeneration,
                    requestId,
                    flags,
                    routeContext.ReplyCapability,
                    routeContext.DeadlineUnixMs)
                : default;
    }

    private async ValueTask FollowAsync(
        ActorQueue queue,
        MessageFollowFrame frame,
        CancellationToken cancellationToken)
    {
        var submitted = false;
        try
        {
            var headerSubmitted = false;
            var firstAttempt = true;
            while (firstAttempt || frame.MessageFollowRoute.Lease.IsActive)
            {
                firstAttempt = false;
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    if (!headerSubmitted)
                    {
                        using var headerPart = Message.From(frame.HeaderBytes);
                        headerSubmitted = _runtime.ForwardActorBoundSessionPart(
                            frame.MessageFollowRoute.TargetMeshName,
                            frame.MessageFollowRoute.TargetActor,
                            frame.MessageFollowRoute.TargetNodeGeneration,
                            frame.MessageFollowRoute.TargetAuthorityOwnerGeneration,
                            frame.MessageFollowRoute.TargetOwnerLeaseGeneration,
                            frame.SourceNodeRid,
                            frame.SourceSessionRid,
                            headerPart,
                            true,
                            SendFlags.DontWait,
                            frame.MessageFollowRouteContext,
                            frame.SourceNodeGeneration,
                            frame.RequestSource,
                            frame.ApplicationMetadata);
                        if (!headerSubmitted)
                        {
                            await DelayRetryAsync(cancellationToken).ConfigureAwait(false);
                            continue;
                        }
                    }

                    using var bodyPart = Message.From(frame.BodyBytes);
                    if (_runtime.ForwardActorBoundSessionPart(
                            frame.MessageFollowRoute.TargetMeshName,
                            frame.MessageFollowRoute.TargetActor,
                            frame.MessageFollowRoute.TargetNodeGeneration,
                            frame.MessageFollowRoute.TargetAuthorityOwnerGeneration,
                            frame.MessageFollowRoute.TargetOwnerLeaseGeneration,
                            frame.SourceNodeRid,
                            frame.SourceSessionRid,
                            bodyPart,
                            false,
                            SendFlags.DontWait,
                            frame.MessageFollowRouteContext,
                            frame.SourceNodeGeneration,
                            frame.RequestSource,
                            frame.ApplicationMetadata))
                    {
                        TrySendMessageFollowNotification(queue, frame);
                        submitted = true;
                        return;
                    }
                }
                catch (ZlinkSubmitException exception)
                    when (exception.Result is ZlinkSubmitException.ErrorCode.Backpressured
                          or ZlinkSubmitException.ErrorCode.InvalidState
                          or ZlinkSubmitException.ErrorCode.NotConnected
                          or ZlinkSubmitException.ErrorCode.NotFound)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"message follow retry: {exception.Message}");
                }
                catch (Exception exception) when (exception is not OperationCanceledException)
                {
                    ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"message follow failed: {exception.Message}");
                    break;
                }

                await DelayRetryAsync(cancellationToken).ConfigureAwait(false);
            }

            await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                    _runtime,
                    frame.MessageFollowRoute.SourceActor,
                    frame.SourceNodeRid,
                    frame.SourceSessionRid,
                    frame.RequestId,
                    frame.Flags,
                    frame.MessageFollowRouteContext.ReplyCapability,
                    frame.Header,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        $"Actor ref '{frame.MessageFollowRoute.SourceActor.ActorId}' could not use Message Follow before its duration expired."),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            frame.Complete(submitted);
        }
    }

    private void TrySendMessageFollowNotification(
        ActorQueue queue,
        MessageFollowFrame frame)
    {
        var source = frame.MessageFollowRoute.SourceActor;
        var target = frame.MessageFollowRoute.TargetActor;
        var fence = new ZLinkMessageFollowFence(
            ZLinkMessageFollowObjectKind.Actor,
            source.ActorId,
            target.ActorId,
            source.NodeRid,
            target.NodeRid,
            source.Generation,
            target.Generation,
            frame.MessageFollowRoute.SourceNodeGeneration,
            frame.MessageFollowRoute.TargetNodeGeneration,
            frame.MessageFollowRoute.SourceAuthorityOwnerGeneration,
            frame.MessageFollowRoute.TargetAuthorityOwnerGeneration,
            frame.MessageFollowRoute.SourceOwnerLeaseGeneration,
            frame.MessageFollowRoute.TargetOwnerLeaseGeneration);
        if (!frame.MessageFollowRoute.Lease.TryBeginMessageFollowNotice(fence))
            return;

        var operationId = frame.MessageFollowRouteContext.OperationId;
        var hopCount = frame.MessageFollowRouteContext.MessageFollowHopCount;
        if (operationId == default
            || frame.SourceNodeRid.IsEmpty
            || hopCount is 0 or > ZLinkServiceWireCodec.MessageFollowMaximumHopCount)
        {
            frame.MessageFollowRoute.Lease.AbortMessageFollowNotice(fence);
            return;
        }

        try
        {
            var record = new ZLinkServiceWireCodec.MessageFollowRecord(
                new ZLinkServiceWireCodec.MessageFollowRoute(
                    ZLinkServiceWireCodec.MessageFollowActorKind,
                    source.ActorId,
                    source.Generation,
                    source.NodeRid,
                    frame.MessageFollowRoute.SourceNodeGeneration,
                    frame.MessageFollowRoute.SourceAuthorityOwnerGeneration,
                    frame.MessageFollowRoute.SourceOwnerLeaseGeneration),
                new ZLinkServiceWireCodec.MessageFollowRoute(
                    ZLinkServiceWireCodec.MessageFollowActorKind,
                    target.ActorId,
                    target.Generation,
                    target.NodeRid,
                    frame.MessageFollowRoute.TargetNodeGeneration,
                    frame.MessageFollowRoute.TargetAuthorityOwnerGeneration,
                    frame.MessageFollowRoute.TargetOwnerLeaseGeneration),
                hopCount,
                queue.SnapshotQueuedMessages(),
                queue.SnapshotQueuedBytes(),
                operationId,
                frame.RequestId == 0 ? 0 : frame.RequestId);
            var node = _runtime
                .GetMeshNodeRuntime(frame.MessageFollowRoute.TargetMeshName)
                .Node;
            if (node is not IZLinkBackendMessageFollowNotifications sender
                || !sender.TrySendMessageFollowNotification(
                    frame.SourceNodeRid,
                    record))
                frame.MessageFollowRoute.Lease.AbortMessageFollowNotice(fence);
            else
                frame.MessageFollowRoute.Lease.MarkMessageFollowNoticeSent(fence);
        }
        catch (Exception exception)
            when (exception is InvalidOperationException
                or ZlinkException
                or ZLinkFrameworkException)
        {
            frame.MessageFollowRoute.Lease.AbortMessageFollowNotice(fence);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"message follow notification failed: {exception.Message}");
        }
    }

    private static ValueTask DelayRetryAsync(CancellationToken cancellationToken)
        => new(Task.Delay(TimeSpan.FromMilliseconds(10), cancellationToken));

    private sealed class ActorQueue(
        ZLinkActorMessageFollower owner,
        MessageFollowKey key)
    {
        private readonly object _lifecycleGate = new();
        private readonly ZLinkSerialExecutionQueue _queue =
            new(
                new ZLinkRuntimeTaskRunner(
                    owner._runtime.ErrorSink,
                    owner._runtime.ShutdownToken,
                    owner._runtime.ExecutionOwner),
                owner._runtime.ErrorSink,
                owner._runtime.ShutdownToken);
        private bool _retired;
        private bool _retirementScheduled;
        private long _queuedBytes;

        internal uint SnapshotQueuedMessages()
        {
            return (uint)_queue.ApplicationPendingCount;
        }

        internal uint SnapshotQueuedBytes()
        {
            return (uint)Math.Min(
                Volatile.Read(ref _queuedBytes),
                uint.MaxValue);
        }

        public bool TryEnqueue(MessageFollowFrame frame)
        {
            lock (_lifecycleGate)
            {
                if (_retired) return false;
                var encodedSize = frame.EncodedSize;
                Interlocked.Add(ref _queuedBytes, encodedSize);
                var admission = _queue.TryPostApplicationWithAdmission(
                    async cancellationToken =>
                    {
                        try
                        {
                            await owner.FollowAsync(
                                    this,
                                    frame,
                                    cancellationToken)
                                .ConfigureAwait(false);
                        }
                        finally
                        {
                            Interlocked.Add(ref _queuedBytes, -encodedSize);
                        }
                    },
                    out _);
                if (admission == ZLinkSerialPostAdmission.Accepted)
                {
                    if (!_retirementScheduled)
                    {
                        _retirementScheduled = true;
                        _ = RetireWhenDrainedAsync();
                    }
                    return true;
                }
                Interlocked.Add(ref _queuedBytes, -encodedSize);
                _retired = true;
                return false;
            }
        }

        private async Task RetireWhenDrainedAsync()
        {
            while (true)
            {
                await _queue.ApplicationDrained.ConfigureAwait(false);
                lock (_lifecycleGate)
                {
                    if (_retired) return;
                    if (_queue.ApplicationPendingCount != 0)
                        continue;
                    _retired = true;
                    owner._queues.TryRemove(
                        new KeyValuePair<MessageFollowKey, ActorQueue>(key, this));
                    break;
                }
            }
            await _queue.DisposeAsync().ConfigureAwait(false);
        }
    }

    private sealed class MessageFollowFrame(
        ZLinkActorMessageFollowRoute messageFollowRoute,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        ulong requestId,
        uint flags,
        ZLinkBackendActorRouteContext routeContext,
        ulong sourceNodeGeneration,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        byte[] applicationMetadata,
        ZlinkStreamHeader header,
        byte[] headerBytes,
        byte[] bodyBytes)
    {
        private readonly TaskCompletionSource<bool> _completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ZLinkActorMessageFollowRoute MessageFollowRoute { get; } = messageFollowRoute;
        public RoutingId SourceNodeRid { get; } = sourceNodeRid;
        public RoutingId SourceSessionRid { get; } = sourceSessionRid;
        public ulong RequestId { get; } = requestId;
        public uint Flags { get; } = flags;
        public ulong SourceNodeGeneration { get; } = sourceNodeGeneration;
        public ZLinkServiceWireCodec.RequestSourceFence? RequestSource { get; } =
            requestSource;
        public byte[] ApplicationMetadata { get; } = applicationMetadata;
        public ZLinkBackendActorRouteContext MessageFollowRouteContext { get; } =
            AdvanceRoute(messageFollowRoute, routeContext, requestId, flags);
        public ZlinkStreamHeader Header { get; } = header;
        public byte[] HeaderBytes { get; } = headerBytes;
        public byte[] BodyBytes { get; } = bodyBytes;
        public long EncodedSize { get; } =
            checked(
                (long)headerBytes.Length
                + bodyBytes.Length
                + applicationMetadata.Length);
        public Task<bool> Completion => _completion.Task;
        public void Complete(bool submitted) =>
            _completion.TrySetResult(submitted);
    }

    private readonly record struct MessageFollowKey(
        RoutingId SourceNodeRid,
        string ActorId,
        ulong ObjectGeneration,
        RoutingId TargetNodeRid,
        ulong TargetObjectGeneration,
        ulong SourceNodeGeneration,
        ulong TargetNodeGeneration,
        ulong SourceAuthorityOwnerGeneration,
        ulong TargetAuthorityOwnerGeneration,
        ulong SourceOwnerLeaseGeneration,
        ulong TargetOwnerLeaseGeneration);

    private readonly record struct DirectReplyKey(
        string ActorId,
        ulong RequestId,
        string Capability);

    private enum DirectReplyDeliveryResult
    {
        Interrupted,
        Submitted,
        TerminalRejected,
        DeadlineExpired
    }

    private sealed class PendingDirectReply(
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult> reply,
        ulong deadlineUnixMs)
    {
        private int _state;

        public Func<IReadOnlyList<Message>, SendFlags, SubmitResult> Reply { get; } =
            reply;
        public ulong DeadlineUnixMs { get; } = deadlineUnixMs;
        public bool HasExplicitDeadline =>
            DeadlineUnixMs is > 0 and <= long.MaxValue;
        public bool IsExpired =>
            Volatile.Read(ref _state) == 2
            || Remaining <= TimeSpan.Zero;
        public TimeSpan Remaining
        {
            get
            {
                if (DeadlineUnixMs is 0 or > long.MaxValue)
                    return TimeSpan.MaxValue;
                var milliseconds = checked(
                    (long)DeadlineUnixMs
                    - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
                return milliseconds <= 0
                    ? TimeSpan.Zero
                    : TimeSpan.FromMilliseconds(milliseconds);
            }
        }

        public bool TryBeginDelivery() =>
            Interlocked.CompareExchange(ref _state, 1, 0) == 0;

        public void ReleaseDelivery()
        {
            Interlocked.CompareExchange(ref _state, 0, 1);
        }

        public void Complete()
        {
            Interlocked.Exchange(ref _state, 2);
        }

        public void Expire()
        {
            Interlocked.Exchange(ref _state, 2);
        }
    }

    internal readonly record struct PreservedDirectReply(
        string Capability,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult> Reply);
}
