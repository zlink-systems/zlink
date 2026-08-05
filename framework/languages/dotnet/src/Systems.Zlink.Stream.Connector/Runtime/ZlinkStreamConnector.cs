using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamConnector : IZlinkStreamConnectorInternal
{
    internal const string ReservedPacketNamePrefix = "$zlink.";
    internal const string HeartbeatPingName = "$zlink.heartbeat.ping";
    internal const string HeartbeatPongName = "$zlink.heartbeat.pong";
    private readonly ZlinkStreamConnectorCallbacks _callbacks;
    private readonly IZlinkStreamCompressionCodec? _compressionCodec;
    private readonly ZlinkStreamFrameSender _frameSender;
    private readonly ZlinkStreamHeaderCodec _headerCodec;
    private readonly ZlinkStreamInboundObserverDispatcher _inboundObservers;
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
        : this(options, token => ZlinkStreamTransportFactory.ConnectAsync(options, token))
    {
    }

    internal ZlinkStreamConnector(
        ZlinkStreamConnectorOptions options,
        Func<CancellationToken, ValueTask<IZlinkStreamConnection>> connectTransport)
    {
        Options = options ?? throw new ArgumentNullException(nameof(options));
        ZlinkStreamConnectorOptionsValidator.Validate(options);
        _taskRunner = new ZlinkStreamTaskRunner(_lifetimeCts.Token);
        _receivedMessages = new ZlinkStreamReceivedMessages(options.MaxReceivedMessages);
        _callbacks = new ZlinkStreamConnectorCallbacks(
            _taskRunner,
            options.DispatchMode,
            options.MaxPendingDispatchCallbacks);
        _inboundObservers = new ZlinkStreamInboundObserverDispatcher(
            _taskRunner,
            _callbacks,
            options.MaxInboundObserverNotifications,
            options.MaxInboundObserverPayloadPreviewBytes);

        _headerCodec = new ZlinkStreamHeaderCodec();
        _compressionCodec = CreateCompressionCodec(options);

        _nameResolver = options.NameResolver;
        _lifecycle = new ZlinkStreamConnectorLifecycle(
            options,
            _pending,
            _taskRunner,
            _callbacks,
            connectTransport);
        _frameSender = new ZlinkStreamFrameSender(
            options,
            _headerCodec,
            _compressionCodec,
            _sendGate,
            () => _lifecycle.Connection);
        _oneWaySubmits = new ZlinkStreamOneWaySubmitQueue(
            _taskRunner,
            _callbacks,
            (frame, cancellationToken) =>
                ((IZlinkStreamConnectorInternal)this).SendFrameAsync(frame, cancellationToken));
        _receiveDispatcher = new ZlinkStreamReceiveDispatcher(
            _headerCodec,
            _pending,
            _typedHandlers,
            _receivedMessages,
            _frameSender,
            _callbacks,
            _inboundObservers,
            _lifecycle.HandleServerCloseAsync);
        _receiveLoop = new ZlinkStreamReceiveLoop(
            _receiveDispatcher,
            () => _lifecycle.Connection,
            _lifecycle.RecordInbound,
            options.MaxReceivePayloadSize);
        Connect = new ZlinkStreamLifecycleCall(ConnectCoreAsync);
        Close = new ZlinkStreamLifecycleCall(CloseCoreAsync);
        Dispatch = new ZlinkStreamLifecycleCall(DispatchCoreAsync);
    }

    public event Func<ZlinkStreamError, CancellationToken, ValueTask>? ErrorReceived
    {
        add => _callbacks.AddErrorReceived(value);
        remove => _callbacks.RemoveErrorReceived(value);
    }

    public event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? Disconnected
    {
        add => _callbacks.AddDisconnected(value);
        remove => _callbacks.RemoveDisconnected(value);
    }

    public event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? ConnectionStateChanged
    {
        add => _callbacks.AddConnectionStateChanged(value);
        remove => _callbacks.RemoveConnectionStateChanged(value);
    }

    public bool IsConnected => _lifecycle.IsConnected;

    public ZlinkStreamConnectionState State => _lifecycle.State;

    public ZlinkStreamConnectorOptions Options { get; }

    public int PendingDispatchCount => _callbacks.PendingDispatchCount;

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

    public IDisposable ObserveInbound(
        Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(observer);
        if (_lifecycle.State != ZlinkStreamConnectionState.Created)
            throw Error(
                ZlinkStreamErrorCode.ValidationFailed,
                "Inbound observers must be registered before connecting.");

        return _inboundObservers.Add(observer);
    }

    public IDisposable On(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
    {
        ArgumentNullException.ThrowIfNull(handler);
        ThrowIfClosed();
        ValidateName(name);

        return _typedHandlers.Add(name, handler);
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

    public ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForEncodedAsync(
        string name,
        Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        ThrowIfDisposed();
        ValidateName(name);
        return _receivedMessages.WaitForAsync(name, predicate, timeout, cancellationToken);
    }

    ZlinkStreamOutboundFrame IZlinkStreamConnectorInternal.BuildSendFrame(
        ZlinkStreamMessageKind kind,
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress)
    {
        var frame = _frameSender.BuildOutboundFrame(kind, name, payload, metadata, compress, null);
        _frameSender.ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
        return frame;
    }

    async ValueTask IZlinkStreamConnectorInternal.SendFrameAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken)
    {
        try
        {
            await _frameSender.SendPacketAsync(frame.HeaderBytes, frame.PayloadBytes, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkStreamException ex)
        {
            await _lifecycle.HandleTransportErrorAsync(ex.Error, cancellationToken).ConfigureAwait(false);
            throw;
        }
    }

    ValueTask IZlinkStreamConnectorInternal.SubmitFrameAsync(
        ZlinkStreamOutboundFrame frame,
        CancellationToken cancellationToken)
    {
        ThrowIfDisposed();
        return _oneWaySubmits.SubmitAsync(frame, cancellationToken);
    }

    async ValueTask<ZlinkStreamEncodedPayload> IZlinkStreamConnectorInternal.RequestEncodedAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var completion = await RequestEncodedCoreAsync(
                name,
                payload,
                metadata,
                compress,
                timeout,
                cancellationToken)
            .ConfigureAwait(false);
        if (completion.Error is { } error) throw new ZlinkStreamException(error);
        return completion.Payload!;
    }

    void IZlinkStreamConnectorInternal.RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        Action<ZlinkStreamResult> callback)
    {
        ThrowIfDisposed();
        _callbacks.QueueRequestCallback(
            () => RequestEncodedCoreAsync(name, payload, metadata, compress, timeout, CancellationToken.None),
            reply => ZlinkStreamResult.Success(),
            ZlinkStreamResult.Failure,
            callback);
    }

    void IZlinkStreamConnectorInternal.RequestEncoded(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback)
    {
        ThrowIfDisposed();
        _callbacks.QueueRequestCallback(
            () => RequestEncodedCoreAsync(name, payload, metadata, compress, timeout, CancellationToken.None),
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success,
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure,
            callback);
    }

    public ValueTask DisposeAsync()
    {
        if (_callbacks.IsCurrentCallback)
            throw new InvalidOperationException(
                "DisposeAsync cannot run inside a connector callback. Use Close.Async in the callback and dispose the connector externally after the callback returns.");

        Task finalizationTask;
        TaskCompletionSource? startFinalization = null;
        lock (_disposeGate)
        {
            if (_finalizationTask is null)
            {
                Volatile.Write(ref _disposed, 1);
                startFinalization = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _finalizationTask = FinalizeAfterStartAsync(startFinalization.Task);
            }

            finalizationTask = _finalizationTask;
        }

        startFinalization?.TrySetResult();
        return new ValueTask(finalizationTask);
    }

    private async Task FinalizeAfterStartAsync(Task started)
    {
        await started.ConfigureAwait(false);
        _oneWaySubmits.Complete();
        try
        {
            // A successful one-way terminal means the frame entered this bounded
            // queue. Dispose must let every accepted frame finish writing before
            // it closes the transport that owns those writes.
            await _oneWaySubmits.WaitForCompletionAsync().ConfigureAwait(false);
            await _lifecycle.CloseAsync(CancellationToken.None).ConfigureAwait(false);
        }
        finally
        {
            await _lifetimeCts.CancelAsync().ConfigureAwait(false);
            await _taskRunner.StopAndDrainAsync().ConfigureAwait(false);
            _callbacks.Complete();
            _inboundObservers.Dispose();
            _sendGate.Dispose();
            _lifecycle.Dispose();
            _lifetimeCts.Dispose();
        }
    }

    private async ValueTask ConnectCoreAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycle.ConnectAsync(
                _receiveLoop.RunAsync,
                SendHeartbeatPingAsync,
                ThrowIfDisposed,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask CloseCoreAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycle.CloseAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask DispatchCoreAsync(CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        await _callbacks.DispatchAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<ZlinkStreamRequestCompletion> RequestEncodedCoreAsync(
        string name,
        ZlinkStreamEncodedPayload payload,
        ZlinkStreamMetadata metadata,
        bool compress,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var pending = _pending.Create(name);
        var frame = _frameSender.BuildOutboundFrame(ZlinkStreamMessageKind.Request, name, payload, metadata, compress,
            pending.RequestSeq);

        try
        {
            _frameSender.ValidateSendReady(frame.HeaderBytes, frame.PayloadBytes);
            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeoutCts.CancelAfter(timeout);
            await _oneWaySubmits.SendAsync(frame, timeoutCts.Token)
                .ConfigureAwait(false);

            var pendingCompletion = await _pending.WaitAsync(pending, timeoutCts.Token).ConfigureAwait(false);
            var replyHeader = pendingCompletion.Header;
            if (pendingCompletion.Error is { } remoteError)
                return new ZlinkStreamRequestCompletion(
                    null,
                    remoteError,
                    replyHeader.FlowId,
                    replyHeader.FlowOrigin);

            var replyBody = _frameSender.DecompressIfNeeded(replyHeader, pendingCompletion.Frame.Payload);
            return new ZlinkStreamRequestCompletion(
                new ZlinkStreamEncodedPayload(replyHeader.Codec, replyBody),
                null,
                replyHeader.FlowId,
                replyHeader.FlowOrigin);
        }
        catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested)
        {
            throw Error(
                ZlinkStreamErrorCode.RequestTimeout,
                "Request timed out.",
                ex);
        }
        finally
        {
            _pending.Remove(pending.RequestSeq);
        }
    }

    private string ResolveName(Type payloadType)
    {
        ThrowIfClosed();
        var name = _nameResolver.Resolve(payloadType);
        return name;
    }

    private string? ResolveNameOrDefault(ZlinkStreamEncodedPayload payload)
    {
        ThrowIfClosed();
        if (payload.MessageType is null) return null;

        return ResolveName(payload.MessageType);
    }

    internal static void ValidateName(
        string name,
        bool allowReserved = false,
        ZlinkStreamErrorCode errorCode = ZlinkStreamErrorCode.ValidationFailed)
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
        if (_disposed != 0) throw new ObjectDisposedException(nameof(ZlinkStreamConnector));
    }

    private void ThrowIfClosed()
    {
        ThrowIfDisposed();
        if (_lifecycle.State == ZlinkStreamConnectionState.Closed)
            throw new ObjectDisposedException(nameof(ZlinkStreamConnector), "Connector is closed.");
    }

    private async ValueTask SendHeartbeatPingAsync(CancellationToken cancellationToken)
    {
        await _frameSender.SendControlAsync(HeartbeatPingName, cancellationToken).ConfigureAwait(false);
    }

    private static IZlinkStreamCompressionCodec? CreateCompressionCodec(ZlinkStreamConnectorOptions options)
    {
        if (options.Compression == ZlinkStreamCompression.None)
        {
            if (options.CompressionCodec is not null)
                throw Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "CompressionCodec cannot be set when Compression is None.");

            return null;
        }

        if (options.CompressionCodec is not null) return options.CompressionCodec;

        return options.Compression switch
        {
            ZlinkStreamCompression.Lz4 => new ZlinkStreamLz4CompressionCodec(),
            _ => throw Error(ZlinkStreamErrorCode.ConfigurationError, "Compression option is not supported.")
        };
    }

    internal static ZlinkStreamException Error(
        ZlinkStreamErrorCode code,
        string message,
        Exception? exception = null)
    {
        return new ZlinkStreamException(new ZlinkStreamError(code, message, exception));
    }
}

internal sealed record ZlinkStreamRequestCompletion(
    ZlinkStreamEncodedPayload? Payload,
    ZlinkStreamError? Error,
    string? FlowId,
    ZlinkStreamFlowOrigin? FlowOrigin);
