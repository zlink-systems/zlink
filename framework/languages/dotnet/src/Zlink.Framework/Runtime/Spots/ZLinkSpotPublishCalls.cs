using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkCurrentSpotPublishCall<TEvent>(
    IZLinkCurrentSpotActivation activation,
    string channelName,
    string topic,
    TEvent message) : IZLinkPublishCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkOneWayCallGate _submission = new("Spot publish");
    private readonly string _messageName = ZLinkMessageNameResolver.ResolveFromMessage(message);

    public IZLinkPublishCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkPublishCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            activation.Flow.CaptureEnabled);
        cancellationToken.ThrowIfCancellationRequested();
        var parts = ZLinkSpotPublishEnvelope.EncodeParts(
            channelName,
            _messageName,
            topic,
            message,
            activation.Codecs);
        var result = await activation.OutboundEndpoint
            .PublishCurrentAsync(
                channelName,
                topic,
                parts,
                cancellationToken,
                _metadata.Encode(),
                () => ZLinkMessageParts.DisposeAll(parts),
                activation.ErrorSink)
            .ConfigureAwait(false);
        ZLinkLogicalMulticastOutcome.EnsureCompleted(result);
    }
}

internal sealed class ZLinkSpotPublisherClientService(ZLinkFrameworkRuntime runtime)
    : IZLinkSpotPublisherClient
{
    public IZLinkPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message)
    {
        return new ZLinkExternalSpotPublishCall<TEvent>(
            runtime, channelName, topic, message);
    }
}

internal sealed class ZLinkExternalSpotPublishCall<TEvent>(
    ZLinkFrameworkRuntime runtime,
    string channelName,
    string topic,
    TEvent message) : IZLinkPublishCall
{
    private readonly ZLinkCallMetadata _metadata = new();
    private readonly ZLinkOneWayCallGate _submission = new("Logical Multicast publish");
    private readonly string _messageName = ZLinkMessageNameResolver.ResolveFromMessage(message);

    public IZLinkPublishCall Metadata(string key, string value)
    {
        _metadata.Set(key, value);
        return this;
    }

    public IZLinkPublishCall Metadata(ZLinkMessageMetadata metadata)
    {
        _metadata.Merge(metadata);
        return this;
    }

    public async ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        _submission.Claim();
        using var operation = runtime.EnterOperation();
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            runtime.Flow.CaptureEnabled);
        var bundle = runtime.GetSpotPublisherBundle(channelName);
        var packetName = _messageName;
        var metadata = _metadata.Encode();
        var parts = ZLinkSpotPublishEnvelope.EncodeParts(
            channelName,
            packetName,
            topic,
            message,
            runtime.Registration.Codecs);
        var backgroundOperation = runtime.RetainOperationForBackgroundWork();
        var released = 0;

        void ReleaseWorkerResources()
        {
            if (Interlocked.Exchange(ref released, 1) != 0) return;
            ZLinkMessageParts.DisposeAll(parts);
            backgroundOperation.Dispose();
        }

        try
        {
            var result = await ZLinkLogicalMulticastSubmitter.SubmitAsync(
                    runtime.LogicalMulticastWorkerPool,
                    () => bundle.Spot.Publish(
                        channelName,
                        topic,
                        parts,
                        SendFlags.None,
                        metadata),
                    cancellationToken,
                    runtime.ShutdownToken,
                    runtime.Registration.DefaultSocketSendTimeout,
                    ReleaseWorkerResources,
                    runtime.ErrorSink)
                .ConfigureAwait(false);
            ZLinkLogicalMulticastOutcome.EnsureCompleted(result);
        }
        catch
        {
            ReleaseWorkerResources();
            throw;
        }
    }
}

internal static class ZLinkLogicalMulticastSubmitter
{
    public static async ValueTask<SubmitResult> SubmitAsync(
        ZLinkWorkerPool workerPool,
        Action publish,
        CancellationToken cancellationToken,
        CancellationToken shutdownToken,
        TimeSpan admissionTimeout,
        Action release,
        IZLinkRuntimeFailureReporter errorSink)
    {
        ArgumentNullException.ThrowIfNull(workerPool);
        ArgumentNullException.ThrowIfNull(publish);
        ArgumentNullException.ThrowIfNull(release);
        ArgumentNullException.ThrowIfNull(errorSink);

        var operation = new LogicalMulticastOperation(
            publish,
            release,
            errorSink,
            cancellationToken,
            shutdownToken);
        if (cancellationToken.IsCancellationRequested)
        {
            operation.CancelBeforeCommit();
            return await operation.Task.ConfigureAwait(false);
        }
        if (shutdownToken.IsCancellationRequested)
        {
            operation.FailShutdownBeforeStart();
            return await operation.Task.ConfigureAwait(false);
        }
        ZLinkWorkerSubmitResult admission;
        using var admissionCancellation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            shutdownToken);
        try
        {
            admission = await workerPool.SubmitDirectAsync(
                    operation.Run,
                    operation.FailShutdownBeforeStart,
                    admissionTimeout,
                    admissionCancellation.Token)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (
            shutdownToken.IsCancellationRequested
            && !cancellationToken.IsCancellationRequested)
        {
            admission = ZLinkWorkerSubmitResult.Stopped;
        }
        operation.ResolveAdmission(admission);
        return await operation.Task.ConfigureAwait(false);
    }

    private sealed class LogicalMulticastOperation
    {
        private const int Pending = 0;
        private const int Committed = 1;
        private const int Terminal = 2;

        private readonly CancellationToken _cancellationToken;
        private readonly IZLinkRuntimeFailureReporter _errorSink;
        private readonly Action _publish;
        private readonly Action _release;
        private readonly CancellationToken _shutdownToken;
        private readonly TaskCompletionSource<SubmitResult> _source = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private CancellationTokenRegistration _cancellationRegistration;
        private CancellationTokenRegistration _shutdownRegistration;
        private int _released;
        private int _state;

        public LogicalMulticastOperation(
            Action publish,
            Action release,
            IZLinkRuntimeFailureReporter errorSink,
            CancellationToken cancellationToken,
            CancellationToken shutdownToken)
        {
            _publish = publish;
            _release = release;
            _errorSink = errorSink;
            _cancellationToken = cancellationToken;
            _shutdownToken = shutdownToken;
            _cancellationRegistration = cancellationToken.Register(
                static state => ((LogicalMulticastOperation)state!).CancelBeforeCommit(),
                this);
            _shutdownRegistration = shutdownToken.Register(
                static state => ((LogicalMulticastOperation)state!).FailShutdownBeforeStart(),
                this);
            if (Volatile.Read(ref _state) == Terminal) DisposeRegistrations();
        }

        public Task<SubmitResult> Task => _source.Task;

        public void ResolveAdmission(ZLinkWorkerSubmitResult admission)
        {
            if (admission == ZLinkWorkerSubmitResult.Accepted) return;
            var result = admission == ZLinkWorkerSubmitResult.Full
                ? SubmitResult.Backpressured
                : SubmitResult.Terminated;
            CompletePending(result);
        }

        public void Run(CancellationToken shutdownToken)
        {
            if (Interlocked.CompareExchange(ref _state, Committed, Pending) != Pending) return;
            DisposeRegistrations();
            _source.TrySetResult(SubmitResult.Ok);
            try
            {
                // ForceStop cancels the pool token before owner disposal. A
                // committed publish has no public failure result, but it must
                // not start target processing after its MeshNode/socket owner
                // has entered forced cleanup.
                if (shutdownToken.IsCancellationRequested) return;
                _publish();
            }
            catch (Exception error)
            {
                _errorSink.ReportRuntimeTaskException(
                    "logical-multicast-publish",
                    error);
            }
            finally
            {
                ReleaseOnce();
                Volatile.Write(ref _state, Terminal);
            }
        }

        public void FailShutdownBeforeStart()
        {
            CompletePending(SubmitResult.Terminated);
        }

        public void CancelBeforeCommit()
        {
            if (Interlocked.CompareExchange(ref _state, Terminal, Pending) != Pending) return;
            _source.TrySetCanceled(_cancellationToken);
            ReleaseOnce();
            DisposeRegistrations();
        }

        private void CompletePending(SubmitResult result)
        {
            if (Interlocked.CompareExchange(ref _state, Terminal, Pending) != Pending) return;
            _source.TrySetResult(result);
            ReleaseOnce();
            DisposeRegistrations();
        }

        private void ReleaseOnce()
        {
            if (Interlocked.Exchange(ref _released, 1) == 0)
                _release();
        }

        private void DisposeRegistrations()
        {
            _cancellationRegistration.Dispose();
            _shutdownRegistration.Dispose();
        }
    }
}
internal static class ZLinkLogicalMulticastOutcome
{
    public static void EnsureCompleted(SubmitResult result)
    {
        switch (result)
        {
            case SubmitResult.Ok:
                return;
            case SubmitResult.Backpressured:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    "Logical Multicast timed out before worker admission completed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            case SubmitResult.Terminated:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    "Logical Multicast was rejected because the runtime is shutting down.");
            default:
                throw new InvalidOperationException(
                    $"Logical Multicast failed with submit result '{result}'.");
        }
    }
}


internal static class ZLinkSpotPublishEnvelope
{
    public static IReadOnlyList<Message> EncodeParts<TEvent>(
        string channelName,
        string messageName,
        string topic,
        TEvent message,
        ZLinkCodecRegistryBuilder? codecs = null,
        string? correlationId = null)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Publish,
            channelName,
            messageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            correlationId,
            null,
            topic,
            null,
            null,
            channelName);
        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            message,
            message?.GetType() ?? typeof(TEvent),
            codecs);
    }
}
