using System.Diagnostics;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnector : IZlinkStreamConnectorInternal
{
    internal const string ReservedPacketNamePrefix = "$zlink.";
    internal const string HeartbeatPingName = "$zlink.heartbeat.ping";
    internal const string HeartbeatPongName = "$zlink.heartbeat.pong";
    private readonly ZlinkStreamConnectorCallbacks _callbacks;
    private readonly ZlinkStreamActors _actors;
    private readonly IZlinkStreamCompressionCodec? _compressionCodec;
    private readonly ZlinkStreamFrameSender _frameSender;
    private readonly ZlinkStreamHeaderCodec _headerCodec;
    private readonly ZlinkStreamConnectorLifecycle _lifecycle;
    private readonly ZlinkStreamOneWaySubmitQueue _oneWaySubmits;
    private readonly CancellationTokenSource _lifetimeCts = new();
    private readonly IZlinkStreamPacketNameResolver _nameResolver;

    private readonly ZlinkStreamPendingRequests _pending = new();
    private readonly ZlinkStreamReceiveDispatcher _receiveDispatcher;
    private readonly ZlinkStreamReceivedMessages _receivedMessages;
    private readonly ZlinkStreamReceiveLoop _receiveLoop;
    private readonly object _disposeGate = new();
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly ZlinkStreamTaskRunner _taskRunner;
    private readonly ZlinkStreamTypedHandlerRegistry _typedHandlers = new();
    private Task? _finalizationTask;
    private int _disposed;

    internal ZlinkStreamConnector(ZlinkStreamConnectorOptions options)
        : this(options, token => ZlinkStreamTransportFactory.ConnectAsync(options, token)) { }

    internal ZlinkStreamConnector(
        ZlinkStreamConnectorOptions options,
        Func<CancellationToken, ValueTask<IZlinkStreamConnection>> connectTransport
    )
    {
        Options = options ?? throw new ArgumentNullException(nameof(options));
        ZlinkStreamConnectorOptionsValidator.Validate(options);
        _taskRunner = new ZlinkStreamTaskRunner(_lifetimeCts.Token);
        _receivedMessages = new ZlinkStreamReceivedMessages();
        _callbacks = new ZlinkStreamConnectorCallbacks(_taskRunner, options.DispatchMode);
        _actors = new ZlinkStreamActors(this, _callbacks);
        _headerCodec = new ZlinkStreamHeaderCodec();
        _compressionCodec = CreateCompressionCodec(options);

        _nameResolver = options.NameResolver;
        _lifecycle = new ZlinkStreamConnectorLifecycle(
            options,
            _pending,
            _taskRunner,
            _callbacks,
            connectTransport,
            _receivedMessages.ResetForConnection,
            async () =>
            {
                await _actors.ConnectionEndedAsync().ConfigureAwait(false);
                _receivedMessages.ConnectionEnded();
            },
            async () =>
            {
                _oneWaySubmits.Complete();
                await _oneWaySubmits.WaitForCompletionAsync().ConfigureAwait(false);
            }
        );
        _frameSender = new ZlinkStreamFrameSender(
            options,
            _headerCodec,
            _compressionCodec,
            _sendGate,
            () => _lifecycle.Connection
        );
        _oneWaySubmits = new ZlinkStreamOneWaySubmitQueue(
            _taskRunner,
            () => _lifecycle.Connection,
            SendFrameAsync
        );
        _receiveDispatcher = new ZlinkStreamReceiveDispatcher(
            _headerCodec,
            _pending,
            _typedHandlers,
            _receivedMessages,
            _frameSender,
            _callbacks,
            _actors,
            _lifecycle.HandleServerCloseAsync
        );
        _receiveLoop = new ZlinkStreamReceiveLoop(
            _receiveDispatcher,
            () => _lifecycle.Connection,
            _lifecycle.RecordInbound,
            options.MaxReceivePayloadSize
        );
        Connect = new ZlinkStreamLifecycleCall(ConnectCoreAsync);
        Close = new ZlinkStreamLifecycleCall(CloseCoreAsync);
        Dispatch = new ZlinkStreamLifecycleCall(DispatchCoreAsync);
    }

    public IDisposable OnErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask> handler)
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        return _callbacks.AddErrorReceived(handler);
    }

    public IDisposable OnDisconnected(
        Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler
    )
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        return _callbacks.AddDisconnected(handler);
    }

    public IDisposable OnConnectionStateChanged(
        Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler
    )
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        return _callbacks.AddConnectionStateChanged(handler);
    }

    public IDisposable OnRequestSending(Action<ZlinkStreamRequestSendingContext> handler)
    {
        ThrowIfDisposed();
        return _callbacks.AddRequestSending(handler);
    }

    public IDisposable OnReplyReceived(
        Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask> handler
    )
    {
        ThrowIfDisposed();
        return _callbacks.AddReplyReceived(handler);
    }

    public bool IsConnected => _lifecycle.IsConnected;

    public ZlinkStreamConnectionState State => _lifecycle.State;

    public ZlinkStreamCloseReason? CloseReason => _lifecycle.LastCloseReason;

    public ZlinkStreamConnectorOptions Options { get; }

    public int PendingDispatchCount => _callbacks.PendingDispatchCount;

    public IReadOnlyList<IZlinkStreamActor> Actors => _actors.Snapshot();

    public IZlinkStreamActor? Actor(string actorId)
    {
        ThrowIfDisposed();
        return _actors.Find(actorId);
    }

    public IDisposable OnActorBound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler)
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        return _actors.OnBound(handler);
    }

    public IDisposable OnActorUnbound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler)
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        return _actors.OnUnbound(handler);
    }

    public IZlinkStreamLifecycleCall Connect { get; }

    public IZlinkStreamLifecycleCall Close { get; }

    public IZlinkStreamLifecycleCall Dispatch { get; }

    public int ReceivedCount(string name)
    {
        ThrowIfDisposed();
        ValidateName(name);
        return _receivedMessages.Count(name);
    }

    public IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload)
    {
        return new ZlinkStreamSendBuilder(this, ResolveNameOrDefault(payload), payload);
    }

    public IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload)
    {
        return new ZlinkStreamRequestBuilder(this, ResolveNameOrDefault(payload), payload);
    }

    public IDisposable On(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler
    )
    {
        if (handler is null)
            throw new ArgumentNullException(nameof(handler));
        ThrowIfDisposed();
        ValidateName(name);

        var registration = _typedHandlers.Add(name, handler);
        // Packets already queued for this name now have a handler (stream-connector spec §10).
        _callbacks.HandlerRegistered();
        return registration;
    }

    public IZlinkStreamWaitCall WaitFor(string name)
    {
        ThrowIfDisposed();
        ValidateName(name);
        return new ZlinkStreamWaitBuilder(this, name);
    }

    public IZlinkStreamExpectNoneCall ExpectNone(string name)
    {
        ThrowIfDisposed();
        ValidateName(name);
        return new ZlinkStreamExpectNoneBuilder(this, name);
    }

    public IZlinkStreamSequenceCall WaitForSequence(string name)
    {
        ThrowIfDisposed();
        ValidateName(name);
        return new ZlinkStreamSequenceBuilder(this, name);
    }

    public ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>?> WaitForEncodedAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken
    )
    {
        ThrowIfClosed();
        ValidateName(name);
        return _receivedMessages.WaitForAsync(name, predicate, timeout, cancellationToken);
    }

    ZlinkStreamOutboundFrame IZlinkStreamConnectorInternal.BuildSendFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        ushort? actorSlot
    )
    {
        var frame = _frameSender.BuildOutboundFrame(
            kind,
            name,
            payload,
            metadata,
            compress,
            null,
            actorSlot
        );
        _frameSender.ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
        return frame;
    }

    private async ValueTask SendFrameAsync(
        IZlinkStreamConnection connection,
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await _frameSender
                .SendPacketAsync(
                    connection,
                    frame.HeaderBytes,
                    frame.PayloadBytes,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (ZlinkStreamException ex)
        {
            if (
                !await _lifecycle
                    .HandleTransportErrorAsync(ex.Error, cancellationToken, connection)
                    .ConfigureAwait(false)
            )
                throw Error(
                    ZlinkStreamErrorCode.Disconnected,
                    "The connection ended while this frame was being written.",
                    ex
                );
            throw;
        }
    }

    ValueTask IZlinkStreamConnectorInternal.SubmitFrameAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken
    )
    {
        ThrowIfDisposed();
        return _oneWaySubmits.SendAsync(frame, cancellationToken);
    }

    async ValueTask<ZlinkStreamEncodedPayload> IZlinkStreamConnectorInternal.RequestEncodedAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        CancellationToken cancellationToken
    )
    {
        var started = Stopwatch.GetTimestamp();
        var requestMetadata = PrepareRequest(name, actorId, metadata);
        var completion = await RequestEncodedCoreAsync(
                name,
                payload,
                requestMetadata,
                actorId,
                started,
                compress,
                timeout,
                actorSlot,
                cancellationToken
            )
            .ConfigureAwait(false);
        if (completion.Error is { } error)
            throw new ZlinkStreamException(error);
        return completion.Payload!;
    }

    void IZlinkStreamConnectorInternal.RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        Action<ZlinkStreamResult> callback
    )
    {
        ThrowIfDisposed();
        var started = Stopwatch.GetTimestamp();
        var requestMetadata = PrepareRequest(name, actorId, metadata);
        _callbacks.QueueRequestCallback(
            () =>
                RequestEncodedCoreAsync(
                    name,
                    payload,
                    requestMetadata,
                    actorId,
                    started,
                    compress,
                    timeout,
                    actorSlot,
                    CancellationToken.None
                ),
            reply => ZlinkStreamResult.Success(),
            ZlinkStreamResult.Failure,
            callback
        );
    }

    void IZlinkStreamConnectorInternal.RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        string? actorId,
        Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback
    )
    {
        ThrowIfDisposed();
        var started = Stopwatch.GetTimestamp();
        var requestMetadata = PrepareRequest(name, actorId, metadata);
        _callbacks.QueueRequestCallback(
            () =>
                RequestEncodedCoreAsync(
                    name,
                    payload,
                    requestMetadata,
                    actorId,
                    started,
                    compress,
                    timeout,
                    actorSlot,
                    CancellationToken.None
                ),
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success,
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure,
            callback
        );
    }

    public ValueTask DisposeAsync()
    {
        if (_callbacks.IsCurrentCallback)
            throw new InvalidOperationException(
                "DisposeAsync cannot run inside a connector callback. Use Close.Async in the callback and dispose the connector externally after the callback returns."
            );

        Task finalizationTask;
        TaskCompletionSource<bool>? startFinalization = null;
        lock (_disposeGate)
        {
            if (_finalizationTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                startFinalization = new TaskCompletionSource<bool>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                _finalizationTask = FinalizeAfterStartAsync(startFinalization.Task);
            }

            finalizationTask = _finalizationTask;
        }

        startFinalization?.TrySetResult(true);
        return new ValueTask(finalizationTask);
    }

    private async Task FinalizeAfterStartAsync(Task started)
    {
        await started.ConfigureAwait(false);
        try
        {
            await CloseCoreAsync(CancellationToken.None).ConfigureAwait(false);
        }
        finally
        {
            _lifetimeCts.Cancel();
            await _taskRunner.StopAndDrainAsync().ConfigureAwait(false);
            _callbacks.Complete();
            _sendGate.Dispose();
            _lifecycle.Dispose();
            _lifetimeCts.Dispose();
        }
    }

    private async ValueTask ConnectCoreAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycle
            .ConnectAsync(
                _receiveLoop.RunAsync,
                SendHeartbeatPingAsync,
                ThrowIfDisposed,
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private async ValueTask CloseCoreAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycle.CloseAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask DispatchCoreAsync(CancellationToken cancellationToken = default)
    {
        // Dispatch does not fail after close (stream-connector spec §12): Manual mode runs
        // the callbacks of the close here, and a disposed connector has none left to run.
        await _callbacks.DispatchAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<ZlinkStreamRequestCompletion> RequestEncodedCoreAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        string? actorId,
        long started,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        CancellationToken cancellationToken
    )
    {
        ZlinkStreamRequestCompletion result;
        try
        {
            result = await SendRequestCoreAsync(
                    name,
                    payload,
                    metadata,
                    compress,
                    timeout,
                    actorSlot,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch (ZlinkStreamException ex)
        {
            // A caller cancellation is not caught here, so it does not run this hook (§5.7).
            await _callbacks
                .NotifyReplyReceivedAsync(
                    new ZlinkStreamReplyReceivedContext(
                        name,
                        actorId,
                        false,
                        null,
                        ex.Error,
                        TimeSpan.FromSeconds(
                            (Stopwatch.GetTimestamp() - started) / (double)Stopwatch.Frequency
                        )
                    ),
                    CancellationToken.None
                )
                .ConfigureAwait(false);
            throw;
        }
        await _callbacks
            .NotifyReplyReceivedAsync(
                new ZlinkStreamReplyReceivedContext(
                    name,
                    actorId,
                    result.Error is null,
                    result.Error is null
                        ? new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
                            name,
                            result.Metadata,
                            result.Payload!,
                            actorId
                        )
                        : null,
                    result.Error,
                    TimeSpan.FromSeconds(
                        (Stopwatch.GetTimestamp() - started) / (double)Stopwatch.Frequency
                    )
                ),
                CancellationToken.None
            )
            .ConfigureAwait(false);
        return result;
    }

    private ZlinkStreamMetadata PrepareRequest(
        string name,
        string? actorId,
        ZlinkStreamMetadata metadata
    )
    {
        ThrowIfDisposed();
        var sending = new ZlinkStreamRequestSendingContext(name, actorId, metadata);
        _callbacks.NotifyRequestSending(sending);
        return sending.Metadata;
    }

    private async ValueTask<ZlinkStreamRequestCompletion> SendRequestCoreAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        ushort? actorSlot,
        CancellationToken cancellationToken
    )
    {
        var pending = _pending.Create(name);
        try
        {
            var frame = _frameSender.BuildOutboundFrame(
                ZlinkStreamMessageKind.Request,
                name,
                payload,
                metadata,
                compress,
                pending.RequestSeq,
                actorSlot
            );
            _frameSender.ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken
            );
            await _oneWaySubmits
                .SubmitRequestAsync(frame, timeoutCts.Token, () => timeoutCts.CancelAfter(timeout))
                .ConfigureAwait(false);

            var pendingCompletion = await _pending
                .WaitAsync(pending, timeoutCts.Token)
                .ConfigureAwait(false);
            var replyHeader = pendingCompletion.Header;
            if (pendingCompletion.Error is { } remoteError)
                return new ZlinkStreamRequestCompletion(null, remoteError, replyHeader.Metadata);

            // The receive path already decompressed the reply.
            return new ZlinkStreamRequestCompletion(
                new ZlinkStreamEncodedPayload(replyHeader.Codec, pendingCompletion.Frame.Payload),
                null,
                replyHeader.Metadata
            );
        }
        catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested)
        {
            throw Error(ZlinkStreamErrorCode.RequestTimeout, "Request timed out.", ex);
        }
        finally
        {
            _pending.Remove(pending.RequestSeq);
        }
    }

    private string ResolveName(Type payloadType)
    {
        var name = _nameResolver.Resolve(payloadType);
        return name;
    }

    private string? ResolveNameOrDefault(ZlinkStreamEncodedPayload payload)
    {
        if (payload.MessageType is null)
            return null;

        return ResolveName(payload.MessageType);
    }

    internal static void ValidateName(
        string name,
        bool allowReserved = false,
        ZlinkStreamErrorCode errorCode = ZlinkStreamErrorCode.ValidationFailed
    )
    {
        if (string.IsNullOrEmpty(name))
            throw Error(errorCode, "Message name must not be empty.");

        if (!allowReserved && name.StartsWith(ReservedPacketNamePrefix, StringComparison.Ordinal))
            throw Error(errorCode, "Message name uses a reserved zlink prefix.");

        if (Encoding.UTF8.GetByteCount(name) > byte.MaxValue)
            throw Error(errorCode, "Message name must not exceed 255 UTF-8 bytes.");
    }

    private void ThrowIfDisposed()
    {
        if (_disposed != 0)
            throw new ObjectDisposedException(nameof(ZlinkStreamConnector));
    }

    private void ThrowIfClosed()
    {
        ThrowIfDisposed();
        if (_lifecycle.State == ZlinkStreamConnectionState.Closed)
            throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");
    }

    private async ValueTask SendHeartbeatPingAsync(CancellationToken cancellationToken)
    {
        await _frameSender
            .SendControlAsync(HeartbeatPingName, cancellationToken)
            .ConfigureAwait(false);
    }

    private static IZlinkStreamCompressionCodec? CreateCompressionCodec(
        ZlinkStreamConnectorOptions options
    )
    {
        if (options.Compression == ZlinkStreamCompression.None)
        {
            if (options.CompressionCodec is not null)
                throw Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "CompressionCodec cannot be set when Compression is None."
                );

            return null;
        }

        if (options.CompressionCodec is not null)
            return options.CompressionCodec;

        return options.Compression switch
        {
            ZlinkStreamCompression.Lz4 => new ZlinkStreamLz4CompressionCodec(),
            _ => throw Error(
                ZlinkStreamErrorCode.ConfigurationError,
                "Compression option is not supported."
            ),
        };
    }

    internal static ZlinkStreamException Error(
        ZlinkStreamErrorCode code,
        string message,
        Exception? exception = null
    )
    {
        return new ZlinkStreamException(new ZlinkStreamError(code, message, exception));
    }
}

internal sealed record ZlinkStreamRequestCompletion(
    ZlinkStreamEncodedPayload? Payload,
    ZlinkStreamError? Error,
    ZlinkStreamMetadata Metadata
);
