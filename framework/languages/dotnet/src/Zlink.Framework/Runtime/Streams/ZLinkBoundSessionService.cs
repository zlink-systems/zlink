using Systems.Zlink.Stream.Connector.Runtime;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkBoundSessionService(
    ZLinkFrameworkRuntime runtime) : IZLinkBoundSessionService
{
    private readonly object _submitterGate = new();
    private readonly Dictionary<string, ZLinkAsyncSubmitter> _submitters =
        new(StringComparer.Ordinal);

    public IZLinkBoundSession Create(string actorId)
    {
        return new ZLinkBoundSession(this, actorId);
    }

    internal IZLinkBoundSessionSendCall Send<TMessage>(
        string actorId,
        TMessage message)
    {
        return new ZLinkBoundSessionSendCall<TMessage>(
            this,
            actorId,
            message);
    }

    public async ValueTask DisconnectAsync(
        string actorId,
        CancellationToken cancellationToken = default)
    {
        using var operation = runtime.EnterOperation();
        if (ZLinkBoundSessionDispatchScope.TryDeferClose(
                actorId,
                ct => DisconnectNowAsync(actorId, ct)))
            return;

        await DisconnectNowAsync(actorId, cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask DisconnectNowAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var route = ResolveSessionRoute(actorId);
        try
        {
            await runtime.CloseActorBoundSessionAsync(actorId, cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            if (runtime.TryGetSessionActorContext(actorId, route.BindingToken, out var context))
                runtime.UnbindSessionActor(actorId, context, route.BindingToken);

            runtime.UnbindActorSession(actorId, route.BindingToken);
        }
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SubmitBoundSessionAsync<TMessage>(
        string actorId,
        string? packetName,
        IReadOnlyDictionary<string, string> metadata,
        TMessage message,
        CancellationToken cancellationToken)
    {
        using var operation = runtime.EnterOperation();
        cancellationToken.ThrowIfCancellationRequested();
        _ = ResolveSessionRoute(actorId);
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        var frame = CreateBoundSessionFrame(
            packetName,
            metadata,
            message,
            runtime.Registration.Codecs);
        try
        {
            if (ZLinkBoundSessionDispatchScope.TryDefer(
                    actorId,
                    ct => SubmitDeferredFrameAsync(actorId, frame, ct)))
            {
                TraceSent(actorId, packetName, frame);
                return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted);
            }
        }
        catch (InvalidOperationException error)
            when (error.Message == "Bound-session deferred submit queue is full.")
        {
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Backpressured);
        }

        var result = await SubmitFrameAsync(actorId, frame, cancellationToken)
            .ConfigureAwait(false);
        if (result.Status == ZLinkOneWaySubmitStatus.Submitted)
            TraceSent(actorId, packetName, frame);
        return result;
    }

    private async ValueTask SubmitDeferredFrameAsync(
        string actorId,
        byte[] frame,
        CancellationToken cancellationToken)
    {
        var result = await SubmitFrameAsync(actorId, frame, cancellationToken)
            .ConfigureAwait(false);
        if (result.Status != ZLinkOneWaySubmitStatus.Submitted)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"Deferred bound-session submit completed with '{result.Status}'.");
    }

    private void TraceSent(string actorId, string? packetName, byte[] frame)
    {
        if (!runtime.Flow.Enabled(ZLinkMessageFlowOutcome.Sent)) return;
        if (!ZLinkStreamFrameCodec.TryDecode(frame, out var headerBytes, out _))
            throw new InvalidOperationException("Actor bound session frame is invalid.");

        var header = ZLinkStreamProtocolDefaults.DecodeHeader(headerBytes.ToArray());
        runtime.Flow.Trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.Sent,
            ZLinkDispatchErrorSurface.StreamSession,
            ZLinkDispatchMessageKind.Send,
            packetName,
            CorrelationId: header.CorrelationId,
            ActorId: actorId));
    }

    private ValueTask<ZLinkOneWaySubmitResult> SubmitFrameAsync(
        string actorId,
        byte[] frame,
        CancellationToken cancellationToken)
    {
        var route = ResolveSessionRoute(actorId);
        var message = Message.From(frame);
        return GetSubmitter(route.MeshName).SubmitAsync(
            new[] { message },
            pending => runtime.SendActorBoundSession(
                actorId,
                pending,
                SendFlags.DontWait),
            cancellationToken);
    }

    private ZLinkAsyncSubmitter GetSubmitter(string meshName)
    {
        lock (_submitterGate)
        {
            if (_submitters.TryGetValue(meshName, out var submitter))
                return submitter;
            submitter = runtime.CreateActorBoundSessionSubmitter(meshName);
            _submitters.Add(meshName, submitter);
            return submitter;
        }
    }

    public ValueTask ResetAsync()
    {
        ZLinkAsyncSubmitter[] submitters;
        lock (_submitterGate)
        {
            submitters = _submitters.Values.ToArray();
            _submitters.Clear();
        }

        return DisposeAllAsync(submitters);
    }

    private static async ValueTask DisposeAllAsync(
        IReadOnlyList<ZLinkAsyncSubmitter> submitters)
    {
        foreach (var submitter in submitters)
            await submitter.DisposeAsync().ConfigureAwait(false);
    }

    private ZLinkActorBoundSession ResolveSessionRoute(string actorId)
    {
        if (runtime.TryGetActorBoundSessionForOutbound(actorId, out var route))
            return route;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"No current session binding exists for actor '{actorId}'.",
            ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private static byte[] CreateBoundSessionFrame<TPayload>(
        string? packetName,
        IReadOnlyDictionary<string, string> metadata,
        TPayload payload,
        ZLinkCodecRegistryBuilder codecs)
    {
        var encoded = ZLinkStreamPacketPayloadCodec.Encode(payload, payload?.GetType() ?? typeof(TPayload), codecs);
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            encoded.Codec,
            MetadataFlags(metadata),
            null,
            packetName ?? throw new InvalidOperationException("Packet name is required."),
            ToStreamMetadata(metadata),
            ZlinkStreamCorrelation.Next());
        return ZLinkStreamFrameCodec.Encode(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            encoded.Payload.Span);
    }

    private static ZlinkStreamHeaderFlags MetadataFlags(IReadOnlyDictionary<string, string> metadata)
    {
        return metadata.Count == 0 ? ZlinkStreamHeaderFlags.None : ZlinkStreamHeaderFlags.HasMetadata;
    }

    private static ZlinkStreamMetadata ToStreamMetadata(IReadOnlyDictionary<string, string> metadata)
    {
        var values = ZlinkStreamMetadata.Empty;
        foreach (var (key, value) in metadata) values = values.With(key, value);

        return values;
    }
}

internal sealed class ZLinkBoundSession(
    ZLinkBoundSessionService service,
    string actorId) : IZLinkBoundSession
{
    public IZLinkBoundSessionSendCall Send<TMessage>(
        TMessage message)
    {
        return service.Send(actorId, message);
    }

    public ValueTask DisconnectAsync(
        CancellationToken cancellationToken = default)
    {
        return service.DisconnectAsync(actorId, cancellationToken);
    }
}

internal sealed class ZLinkBoundSessionSendCall<TMessage>(
    ZLinkBoundSessionService service,
    string actorId,
    TMessage message) : IZLinkBoundSessionSendCall
{
    private readonly Dictionary<string, string> _metadata = new(StringComparer.Ordinal);
    private readonly ZLinkOneWayCallGate _submission = new("Bound session send");
    public IZLinkBoundSessionSendCall Metadata(
        string key,
        string value)
    {
        _metadata[key] = value;
        return this;
    }

    public IZLinkBoundSessionSendCall Metadata(ZLinkMessageMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        foreach (var (key, value) in metadata.Values) _metadata[key] = value;
        return this;
    }

    public ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        return service.SubmitBoundSessionAsync(
            actorId,
            ZLinkMessageNameResolver.ResolveFromMessage(message),
            _metadata,
            message,
            cancellationToken).EnsureAcceptedAsync(
                "Bound session send",
                ZLinkFrameworkErrorKind.InvalidOperation);
    }
}
