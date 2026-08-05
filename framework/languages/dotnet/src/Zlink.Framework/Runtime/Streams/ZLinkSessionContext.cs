using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionContext : IZLinkSessionContext
{
    private readonly ZLinkSessionActorsContext _actorSurface;
    private readonly ZLinkSessionClientContext _client;
    private readonly Func<ValueTask> _closeAsync;
    private readonly Func<CancellationToken, ValueTask> _closeByProxyAsync;
    private readonly IZLinkStream _stream;
    private readonly ZLinkAsyncSubmitter? _sendSubmitter;
    private ZLinkSessionDispatchContext? _currentDispatch;
    private ZLinkCompletionAdmissionOwner.ResponderLease? _currentCompletionPermit;
    private ZLinkSessionStreamTransport? _transport;

    public ZLinkSessionContext(
        ZLinkFrameworkRuntime runtime,
        IZLinkStream stream,
        IZLinkSessionHandlerRegistry handlers,
        Func<ValueTask> closeAsync,
        Func<CancellationToken, ValueTask> closeByProxyAsync,
        bool actorDispatchEnabled = true,
        ZLinkAsyncSubmitter? sendSubmitter = null)
    {
        Runtime = runtime;
        _stream = stream;
        Handlers = handlers;
        _closeAsync = closeAsync;
        _closeByProxyAsync = closeByProxyAsync;
        _sendSubmitter = sendSubmitter;
        ActorCoordinator = new ZLinkSessionActorCoordinator(runtime, stream, actorDispatchEnabled);
        _client = new ZLinkSessionClientContext(this);
        _actorSurface = new ZLinkSessionActorsContext(this, ActorCoordinator);
    }

    private ZLinkSessionStreamTransport Transport
        => _transport ??= new ZLinkSessionStreamTransport(_stream, TraceWritten);

    internal ZLinkFrameworkRuntime Runtime { get; }

    internal ZLinkSessionActorCoordinator ActorCoordinator { get; }

    internal ZLinkCodecRegistryBuilder Codecs => Runtime.Registration.Codecs;
    internal IZlinkStreamCompressionCodec? CompressionCodec => Runtime.Registration.StreamCompressionCodec;

    internal ZlinkStreamHeader? CurrentDispatchHeader =>
        _currentDispatch?.RuntimeState as ZlinkStreamHeader;
    internal ZLinkSessionDispatchContext? CurrentDispatchContext => _currentDispatch;

    public string SessionId => _stream.SessionId;

    public RoutingId? RoutingId => _stream.RoutingId;

    public string? LocalAddr => _stream.LocalAddr;

    public string? RemoteAddr => _stream.RemoteAddr;

    public IZLinkSessionClient Client => _client;

    public IZLinkSessionActors Actors => _actorSurface;

    public IZLinkSessionHandlerRegistry Handlers { get; }

    public ValueTask CloseAsync()
    {
        return _closeAsync();
    }

    internal ValueTask CloseByProxyAsync(CancellationToken cancellationToken = default)
    {
        return _closeByProxyAsync(cancellationToken);
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> RelayActorRefAsync(
        ZLinkSessionActor actor,
        Message payload,
        CancellationToken cancellationToken)
    {
        var dispatch = _currentDispatch
                       ?? throw new InvalidOperationException(
                           "Session actor relay requires an active stream dispatch.");
        var header = dispatch.RuntimeState as ZlinkStreamHeader
                     ?? throw new InvalidOperationException("Session actor relay requires runtime dispatch state.");

        try
        {
            await ActorCoordinator.RelayToActorAsync(
                    actor,
                    header,
                    payload,
                    ReplyActorRawAsync,
                    cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.Submitted);
        }
        catch (TimeoutException)
        {
            return new ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus.TimedOut);
        }
        catch (ZLinkFrameworkException failure)
            when (ZLinkMeshCallSupport.TryMapSubmitFailure(failure, out var failed))
        {
            return failed;
        }
        finally
        {
            payload.Dispose();
        }
    }

    internal async ValueTask NotifyActorRefDisconnectedAsync(
        ZLinkSessionActor actor,
        CancellationToken cancellationToken)
    {
        await ActorCoordinator.NotifyActorDisconnectedAsync(actor, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask CleanupAsync(CancellationToken cancellationToken)
    {
        await ActorCoordinator.CleanupAsync(this, cancellationToken).ConfigureAwait(false);
        if (_stream.RoutingId is { } sessionRid) Runtime.CleanupActorSessionsForSession(sessionRid);
    }

    internal ZLinkSessionDispatchContext EnterDispatch(
        ZlinkStreamHeader header,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit = null)
    {
        var metadata = header.Metadata.Count == 0
            ? ZLinkMessageMetadata.Empty
            : new ZLinkMessageMetadata(
                new Dictionary<string, string>(
                    header.Metadata.Values,
                    StringComparer.Ordinal));
        _currentDispatch = new ZLinkSessionDispatchContext(
            header.Name,
            metadata,
            header.RequestSeq.HasValue,
            header);
        _currentCompletionPermit = completionPermit;
        return _currentDispatch;
    }

    internal void ExitDispatch()
    {
        _currentDispatch = null;
        _currentCompletionPermit = null;
    }

    internal bool Write(Message payload)
    {
        return Transport.Write(payload);
    }

    internal async ValueTask<ZLinkOneWaySubmitResult> SubmitAsync(
        Message payload,
        CancellationToken cancellationToken,
        bool isReply = false)
    {
        var completionPermit = isReply ? _currentCompletionPermit : null;
        if (completionPermit is not null)
            await completionPermit.ReserveReplyAsync(
                    checked((ulong)Math.Max(payload.Size, 1)),
                    cancellationToken)
                .ConfigureAwait(false);

        try
        {
            ZLinkOneWaySubmitResult result;
            if (_sendSubmitter is null)
            {
                try
                {
                    result = new ZLinkOneWaySubmitResult(
                        Write(payload)
                            ? ZLinkOneWaySubmitStatus.Submitted
                            : ZLinkOneWaySubmitStatus.Backpressured);
                }
                finally
                {
                    payload.Dispose();
                }
            }
            else
            {
                result = await _sendSubmitter.SubmitSingleAsync(
                        payload,
                        pending => Write(pending),
                        cancellationToken)
                    .ConfigureAwait(false);
            }

            if (completionPermit is not null)
            {
                if (result.Status == ZLinkOneWaySubmitStatus.Submitted)
                    completionPermit.TransferToCore();
                else
                    completionPermit.Dispose();
            }
            return result;
        }
        catch
        {
            completionPermit?.Dispose();
            throw;
        }
    }

    internal void TraceWritten(ZlinkStreamHeader header)
    {
        var outcome = header.Kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error
            ? ZLinkMessageFlowOutcome.Replied
            : ZLinkMessageFlowOutcome.Sent;
        var flow = Runtime.Flow;
        if (!flow.Enabled(outcome)) return;

        var messageKind = header.Kind switch
        {
            ZlinkStreamMessageKind.Response => ZLinkDispatchMessageKind.Response,
            ZlinkStreamMessageKind.Error => ZLinkDispatchMessageKind.Error,
            _ => ZLinkDispatchMessageKind.Send
        };
        flow.Trace(new ZLinkMessageFlowEvent(
            outcome,
            ZLinkDispatchErrorSurface.StreamSession,
            messageKind,
            header.Name,
            CorrelationId: header.CorrelationId,
            SourceRid: RoutingId?.ToString()));
    }

    internal ValueTask ReplyActorRawAsync(
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        CancellationToken cancellationToken)
    {
        if (!TryClaimReply(requestHeader))
            throw new InvalidOperationException("The reply token has already been used.");
        return Transport.ReplyRawAsync(
            requestHeader,
            reply,
            cancellationToken,
            _currentCompletionPermit);
    }

    internal ValueTask ReplyErrorAsync(
        ZlinkStreamHeader requestHeader,
        Exception exception,
        CancellationToken cancellationToken)
    {
        return Transport.ReplyErrorAsync(
            requestHeader,
            exception,
            cancellationToken,
            _currentCompletionPermit);
    }

    private bool TryClaimReply(ZlinkStreamHeader requestHeader)
    {
        return _currentDispatch is { } dispatch
               && ReferenceEquals(dispatch.RuntimeState, requestHeader)
               && dispatch.TryClaimReply();
    }
}

internal sealed class ZLinkSessionClientContext(ZLinkSessionContext context) : IZLinkSessionClient
{
    public IZLinkSessionSendCall Send<TMessage>(TMessage message)
    {
        return new ZLinkSessionSendCall<TMessage>(context, message);
    }

    public IZLinkSessionReplyCall Reply<TMessage>(TMessage message)
    {
        return new ZLinkSessionReplyCall<TMessage>(context, message);
    }
}

internal sealed class ZLinkSessionActorsContext(
    ZLinkSessionContext context,
    ZLinkSessionActorCoordinator actors) : IZLinkSessionActors
{
    public IReadOnlyCollection<IZLinkSessionActor> Bound => actors.BoundActors;

    public ValueTask<IZLinkSessionActor> BindAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default)
    {
        return actors.BindActorAsync(context, actor, cancellationToken);
    }

    public ValueTask<IZLinkSessionActor> BindOrGetAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default)
    {
        return actors.BindOrGetActorAsync(context, actor, cancellationToken);
    }

    public IZLinkSessionActor? Find(string actorId)
    {
        return actors.FindActor(actorId);
    }
}
