using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelReceiveLoop(
    ZLinkFanoutPacketDispatcher dispatcher,
    ZLinkClientServerDispatcher clientServerDispatcher
)
{
    // The bundle owns this logical source and closes its router after source removal.
    // Readiness and post-admission resume both enter the process receive role.
    public async ValueTask RunClientServerLoop(
        string channelName,
        IRouterSocket router,
        ZLinkClientServerServerIdentity identity,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken,
        CancellationToken forceStopToken
    )
    {
        await using var receivePoller = ZLinkBackendSocketPoller.Create(router);
        var applicationDispatch =
            new ZLinkChannelApplicationDispatchQueue<ClientServerDispatchWork>(
                $"client-server-application:{channelName}",
                errorSink,
                cancellationToken,
                forceStopToken,
                DispatchClientServerAsync,
                RejectClientServerDispatch
            );
        try
        {
            var receiveStoragePool = new ZLinkReceivedStoragePool();
            await identity.AttachRouterAsync(router).ConfigureAwait(false);
            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    // Readiness belongs to the source lifetime. Only a received
                    // record enters the recoverable dispatch boundary below.
                    var nextLiveness = await identity
                        .TickLivenessAsync(router, receivePoller, cancellationToken)
                        .ConfigureAwait(false);
                    if (
                        !IsReadable(
                            await receivePoller
                                .WaitAsync(nextLiveness, cancellationToken)
                                .ConfigureAwait(false)
                        )
                    )
                        continue;
                    var reservation = default(ZLinkApplicationJobQueueReservation);
                    try
                    {
                        reservation = await applicationJobQueue
                            .AcquireBatchAsync(
                                ZLinkReceiveBatchBudget.MaximumRecords,
                                cancellationToken,
                                ZLinkApplicationJobOrigin.Remote
                            )
                            .ConfigureAwait(false);
                        var startedAt = System.Diagnostics.Stopwatch.GetTimestamp();
                        var records = 0;
                        long bytes = 0;
                        while (reservation.Count != 0 && !cancellationToken.IsCancellationRequested)
                        {
                            if (ZLinkReceiveBatchBudget.IsExhausted(records, bytes, startedAt))
                                break;
                            await receivePoller.ResumeAsync().ConfigureAwait(false);
                            Received? received = null;
                            ZLinkApplicationJobQueueLease? admission = null;
                            try
                            {
                                received = receiveStoragePool.Rent();
                                if (!router.Recv(received, RecvFlags.DontWait))
                                    break;
                                admission = reservation.Claim()!;
                                records++;
                                bytes = checked(
                                    bytes + ZLinkReceiveBatchBudget.MeasureParts(received.Parts)
                                );

                                if (
                                    received.ReplyToken is null
                                    && received.Parts.Count == 1
                                    && received.Parts[0].Size == 0
                                )
                                    continue;
                                if (received.RoutingId is not { } applicationSource)
                                    continue;
                                var admitted = await identity
                                    .ObserveReceivedRecordAsync(
                                        applicationSource,
                                        received.RouteGeneration,
                                        System.Diagnostics.Stopwatch.GetTimestamp()
                                    )
                                    .ConfigureAwait(false);
                                if (ZLinkClientServerControlProtocol.IsControl(received.Parts))
                                {
                                    ReplyClientServerControl(
                                        router,
                                        received,
                                        identity,
                                        errorSink,
                                        cancellationToken
                                    );
                                    continue;
                                }
                                if (!admitted.Found)
                                    continue;
                                var admittedMaximumMessageBytes = admitted.MaximumMessageBytes;
                                if (
                                    !ZLinkClientServerMessageBound.Fits(
                                        received.Parts,
                                        admittedMaximumMessageBytes
                                    )
                                )
                                {
                                    clientServerDispatcher.RejectMessageTooLarge(
                                        channelName,
                                        router,
                                        received,
                                        applicationDispatch.ReplyGate,
                                        admittedMaximumMessageBytes
                                    );
                                    continue;
                                }
                                var owned = received;
                                var envelope = clientServerDispatcher.ReadHeader(
                                    received.Parts,
                                    channelName,
                                    admission
                                );
                                _ = await applicationDispatch
                                    .PostAsync(
                                        new ClientServerDispatchWork(
                                            channelName,
                                            router,
                                            owned,
                                            receiveStoragePool,
                                            applicationDispatch.ReplyGate,
                                            admittedMaximumMessageBytes,
                                            admission,
                                            envelope
                                        ),
                                        cancellationToken
                                    )
                                    .ConfigureAwait(false);
                                // PostAsync either handed ownership to the queue or ran the
                                // rejection callback. Keep the storage local until that result is
                                // known so a pre-handoff exception is returned by this loop's
                                // finally block.
                                received = null;
                                admission = null;
                            }
                            catch (Exception) when (cancellationToken.IsCancellationRequested)
                            {
                                throw;
                            }
                            catch (ObjectDisposedException)
                            {
                                return;
                            }
                            catch (Exception exception)
                            {
                                errorSink.ReportRuntimeTaskException(
                                    $"client-server-dispatch:{channelName}",
                                    exception
                                );
                                break;
                            }
                            finally
                            {
                                if (received is { } storage)
                                    receiveStoragePool.Return(storage);
                                admission?.Dispose();
                            }
                        }
                    }
                    finally
                    {
                        reservation.Dispose();
                    }
                }
            }
            catch (Exception) when (cancellationToken.IsCancellationRequested) { }
            finally
            {
                await identity.DetachRouterAsync(router).ConfigureAwait(false);
            }
        }
        finally
        {
            // Accepted handlers and their replies finish before the router closes
            // (client-server channel section 6, step 4).
            await applicationDispatch.DisposeAsync().ConfigureAwait(false);
        }
    }

    private void RejectClientServerDispatch(ClientServerDispatchWork work)
    {
        try
        {
            work.ReceiveStoragePool.Return(work.Received);
        }
        finally
        {
            work.Admission.Dispose();
        }
    }

    private async ValueTask DispatchClientServerAsync(
        ClientServerDispatchWork work,
        CancellationToken cancellationToken
    )
    {
        try
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(work.Admission);
            await clientServerDispatcher
                .DispatchAsync(
                    work.ChannelName,
                    work.Router,
                    work.Received,
                    work.ReplyGate,
                    work.MaximumMessageBytes,
                    cancellationToken,
                    work.Envelope
                )
                .ConfigureAwait(false);
        }
        finally
        {
            work.ReceiveStoragePool.Return(work.Received);
        }
    }

    private static void ReplyClientServerControl(
        IRouterSocket router,
        Received received,
        ZLinkClientServerServerIdentity identity,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken
    )
    {
        if (received.RoutingId is not { } sourceRid)
            return;
        if (ZLinkClientServerControlProtocol.TryDecodeLivenessAck(received.Parts, out var ackId))
        {
            AwaitStateLane(
                identity.AcceptLivenessAckAsync(sourceRid, received.RouteGeneration, ackId)
            );
            return;
        }
        if (
            ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(received.Parts, out var probeId)
        )
        {
            identity.RecordLivenessProbe(sourceRid);
            var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(probeId);
            if (received.ReplyToken is not null)
                ReplyOwned(router, sourceRid, received.ReplyToken, ack);
            else
                ZLinkUnawaitedSubmit.Observe(
                    SendOwnedAsync(router, sourceRid, ack, cancellationToken),
                    nameof(ReplyClientServerControl),
                    errorSink
                );
            return;
        }
        var snapshot = AwaitStateLane(identity.ReadAsync());
        var valid = ZLinkClientServerControlProtocol.TryDecodeHello(received.Parts, out var hello);
        var accepted =
            valid
            && hello is not null
            && StringComparer.Ordinal.Equals(hello.ChannelName, identity.ChannelName.Value)
            && ZLinkClientServerControlProtocol.SecurityIdentityMatches(
                identity.SecurityIdentity,
                hello.SecurityIdentity
            );
        var negotiatedMaximumMessageBytes = accepted
            ? Math.Min(
                hello!.NormalizedEffectiveMaxMessageBytes,
                identity.NormalizedEffectiveMaxMessageBytes
            )
            : 0;
        var reply = accepted
            ? ZLinkClientServerControlProtocol.EncodeAdmission(
                identity.ToAdmission(snapshot) with
                {
                    NormalizedEffectiveMaxMessageBytes = negotiatedMaximumMessageBytes,
                }
            )
            : ZLinkClientServerControlProtocol.EncodeReject(reason: 1);
        if (ReplyOwned(router, sourceRid, received.ReplyToken, reply) && accepted)
            AwaitStateLane(
                identity.AdmitPeerAsync(
                    sourceRid,
                    negotiatedMaximumMessageBytes,
                    received.RouteGeneration
                )
            );
    }

    private static bool ReplyOwned(
        IRouterSocket router,
        RoutingId sourceRid,
        ReplyToken? replyToken,
        Message reply
    )
    {
        if (replyToken is not { } value)
        {
            reply.Dispose();
            return false;
        }
        try
        {
            router.Reply(sourceRid, value).Message(reply).Submit();
            return true;
        }
        catch
        {
            reply.Dispose();
            throw;
        }
    }

    private static async ValueTask SendOwnedAsync(
        IRouterSocket router,
        RoutingId sourceRid,
        Message message,
        CancellationToken cancellationToken
    )
    {
        try
        {
            await router
                .Send(sourceRid)
                .Message(message)
                .Async(cancellationToken)
                .EnsureAcceptedAsync()
                .ConfigureAwait(false);
        }
        finally
        {
            message.Dispose();
        }
    }

    // The subscriber bundle closes its SUB after this logical source ends.
    public async ValueTask RunSubscriberLoop(
        string channelName,
        ISubSocket subscriber,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken,
        CancellationToken forceStopToken
    )
    {
        var applicationDispatch = new ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork>(
            $"fanout-application:{channelName}",
            errorSink,
            cancellationToken,
            forceStopToken,
            DispatchFanoutAsync,
            RejectFanoutDispatch
        );
        try
        {
            await RunFanoutReceive(
                    channelName,
                    subscriber,
                    onActivity: null,
                    onProtocolError: () =>
                        // A reserved topic carrying anything else is a protocol error. Leaving
                        // the loop closes this publisher's socket, which is the only connection
                        // it governs.
                        errorSink.ReportRuntimeTaskException(
                            $"channel-subscriber:{channelName}",
                            new InvalidOperationException(
                                "Fanout publisher sent a malformed liveness beacon."
                            )
                        ),
                    applicationJobQueue,
                    applicationDispatch,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        finally
        {
            await applicationDispatch.DisposeAsync().ConfigureAwait(false);
        }
    }

    // The automatic connection closes its SUB after this logical source ends.
    public ValueTask RunFanoutConnectionLoop(
        string channelName,
        ISubSocket subscriber,
        Action onActivity,
        Action onProtocolError,
        ZLinkApplicationJobQueue applicationJobQueue,
        ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork> applicationDispatch,
        CancellationToken cancellationToken
    ) =>
        RunFanoutReceive(
            channelName,
            subscriber,
            onActivity,
            onProtocolError,
            applicationJobQueue,
            applicationDispatch,
            cancellationToken
        );

    private async ValueTask RunFanoutReceive(
        string channelName,
        ISubSocket subscriber,
        Action? onActivity,
        Action onProtocolError,
        ZLinkApplicationJobQueue applicationJobQueue,
        ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork> applicationDispatch,
        CancellationToken cancellationToken
    )
    {
        await using var receivePoller = ZLinkBackendSocketPoller.Create(subscriber);
        var topicMessagePool = new ZLinkTopicMessageStoragePool();
        TopicMessage? topicMessage = topicMessagePool.Rent();
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var reservation = default(ZLinkApplicationJobQueueReservation);
                try
                {
                    if (
                        !IsReadable(
                            await receivePoller
                                .WaitAsync(Timeout.InfiniteTimeSpan, cancellationToken)
                                .ConfigureAwait(false)
                        )
                    )
                        continue;
                    reservation = await applicationJobQueue
                        .AcquireBatchAsync(
                            ZLinkReceiveBatchBudget.MaximumRecords,
                            cancellationToken,
                            ZLinkApplicationJobOrigin.Remote
                        )
                        .ConfigureAwait(false);
                    await receivePoller.ResumeAsync().ConfigureAwait(false);
                    var startedAt = System.Diagnostics.Stopwatch.GetTimestamp();
                    long bytes = 0;
                    for (
                        var count = 0;
                        reservation.Count != 0 && !cancellationToken.IsCancellationRequested;
                        count++
                    )
                    {
                        if (ZLinkReceiveBatchBudget.IsExhausted(count, bytes, startedAt))
                            break;
                        await receivePoller.ResumeAsync().ConfigureAwait(false);
                        ZLinkApplicationJobQueueLease? admission = null;
                        try
                        {
                            if (!subscriber.Subscribe(topicMessage!, RecvFlags.DontWait))
                                break;
                            admission = reservation.Claim()!;
                            bytes = checked(
                                bytes + ZLinkReceiveBatchBudget.MeasureParts(topicMessage!.Parts)
                            );
                            // Beacons share the PUB socket but never enter application queues.
                            if (ZLinkFanoutLivenessProtocol.IsReservedTopic(topicMessage.Topic))
                            {
                                if (!ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                                {
                                    onProtocolError();
                                    return;
                                }
                                onActivity?.Invoke();
                                topicMessagePool.Return(topicMessage);
                                topicMessage = null;
                                topicMessage = topicMessagePool.Rent();
                                continue;
                            }
                            onActivity?.Invoke();
                            var envelope = dispatcher.ReadHeader(topicMessage.Parts);
                            if (envelope.Header is not null)
                                admission.MarkQueued();
                            _ = await applicationDispatch
                                .PostAsync(
                                    new FanoutDispatchWork(
                                        channelName,
                                        topicMessage,
                                        topicMessagePool,
                                        admission,
                                        envelope
                                    ),
                                    cancellationToken
                                )
                                .ConfigureAwait(false);
                            topicMessage = null;
                            admission = null;
                            topicMessage = topicMessagePool.Rent();
                        }
                        finally
                        {
                            admission?.Dispose();
                        }
                    }
                }
                catch (Exception) when (cancellationToken.IsCancellationRequested)
                {
                    break;
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                finally
                {
                    reservation.Dispose();
                }
            }
        }
        finally
        {
            if (topicMessage is { } storage)
                topicMessagePool.Return(storage);
        }
    }

    private static void RejectFanoutDispatch(FanoutDispatchWork work)
    {
        work.TopicMessagePool.Return(work.TopicMessage);
        work.Admission.Dispose();
    }

    private async ValueTask DispatchFanoutAsync(
        FanoutDispatchWork work,
        CancellationToken cancellationToken
    )
    {
        try
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(work.Admission);
            await dispatcher
                .DispatchEventMessageAsync(
                    work.ChannelName,
                    work.TopicMessage,
                    cancellationToken,
                    work.Envelope
                )
                .ConfigureAwait(false);
        }
        finally
        {
            work.TopicMessagePool.Return(work.TopicMessage);
        }
    }

    private static bool IsReadable(ZLinkBackendSocketReadiness readiness) =>
        (
            readiness
            & (
                ZLinkBackendSocketReadiness.Readable
                | ZLinkBackendSocketReadiness.Error
                | ZLinkBackendSocketReadiness.Priority
            )
        ) != 0;

    private readonly record struct ClientServerDispatchWork(
        string ChannelName,
        IRouterSocket Router,
        Received Received,
        ZLinkReceivedStoragePool ReceiveStoragePool,
        ZLinkChannelReplyGate ReplyGate,
        uint MaximumMessageBytes,
        ZLinkApplicationJobQueueLease Admission,
        (ZLinkEnvelopeHeader? Header, ZLinkEnvelopeProtocolException? Error) Envelope
    );

    internal ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork> CreateFanoutDispatchQueue(
        string channelName,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken,
        CancellationToken forceStopToken
    ) =>
        new(
            $"fanout-application:{channelName}",
            errorSink,
            cancellationToken,
            forceStopToken,
            DispatchFanoutAsync,
            RejectFanoutDispatch
        );

    internal readonly record struct FanoutDispatchWork(
        string ChannelName,
        TopicMessage TopicMessage,
        ZLinkTopicMessageStoragePool TopicMessagePool,
        ZLinkApplicationJobQueueLease Admission,
        (ZLinkEnvelopeHeader? Header, ZLinkEnvelopeProtocolException? Error) Envelope
    );
}
