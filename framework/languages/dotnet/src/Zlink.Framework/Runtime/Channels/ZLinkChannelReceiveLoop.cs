using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelReceiveLoop(
    ZLinkFanoutPacketDispatcher dispatcher,
    ZLinkClientServerDispatcher clientServerDispatcher
)
{
    private static readonly TimeSpan ReceivePollInterval = TimeSpan.FromMilliseconds(100);

    // Runs on the dedicated receive thread of the server bundle
    // (ZLinkChannelRuntimeBundle.StartReceiveLoop), which closes the router after this
    // method returns. The loop waits synchronously and never resumes on a ThreadPool worker.
    public void RunClientServerLoop(
        string channelName,
        IRouterSocket router,
        ZLinkClientServerServerIdentity identity,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken,
        CancellationToken forceStopToken
    )
    {
        using var receivePoller = ZLinkBackendSocketPoller.Create(router);
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
            AwaitStateLane(identity.AttachRouterAsync(router));
            try
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    Received? received = null;
                    ZLinkApplicationJobQueueLease? admission = null;
                    try
                    {
                        identity
                            .TickLivenessAsync(router, cancellationToken)
                            .AsTask()
                            .GetAwaiter()
                            .GetResult();
                        if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                            continue;
                        admission = applicationJobQueue
                            .AcquireAsync(cancellationToken, ZLinkApplicationJobOrigin.Remote)
                            .GetAwaiter()
                            .GetResult();
                        received = receiveStoragePool.Rent();
                        if (!router.Recv(received, RecvFlags.DontWait))
                            continue;

                        if (
                            received.ReplyToken is null
                            && received.Parts.Count == 1
                            && received.Parts[0].Size == 0
                        )
                            continue;
                        if (received.RoutingId is not { } applicationSource)
                            continue;
                        var admitted = AwaitStateLane(
                            identity.ObserveReceivedRecordAsync(
                                applicationSource,
                                received.RouteGeneration,
                                System.Diagnostics.Stopwatch.GetTimestamp()
                            )
                        );
                        if (ZLinkClientServerControlProtocol.IsControl(received.Parts))
                        {
                            ReplyClientServerControl(router, received, identity, cancellationToken);
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
                        _ = applicationDispatch
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
                            .GetAwaiter()
                            .GetResult();
                        // PostAsync either handed ownership to the queue or ran the
                        // rejection callback. Keep the storage local until that result is
                        // known so a pre-handoff exception is returned by this loop's
                        // finally block.
                        received = null;
                        admission = null;
                    }
                    catch (Exception) when (cancellationToken.IsCancellationRequested)
                    {
                        break;
                    }
                    catch (ObjectDisposedException)
                    {
                        break;
                    }
                    catch (Exception exception)
                    {
                        errorSink.ReportRuntimeTaskException(
                            $"client-server-dispatch:{channelName}",
                            exception
                        );
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
                AwaitStateLane(identity.DetachRouterAsync(router));
            }
        }
        finally
        {
            // Accepted handlers and their replies finish before the router closes
            // (client-server channel section 6, step 4).
            applicationDispatch.DisposeAsync().AsTask().GetAwaiter().GetResult();
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
                SendOwnedAsync(router, sourceRid, ack, cancellationToken)
                    .AsTask()
                    .GetAwaiter()
                    .GetResult();
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

    // Runs on the dedicated receive thread of the subscriber bundle, which closes the SUB
    // socket after this method returns.
    public void RunSubscriberLoop(
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
            RunFanoutReceive(
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
            );
        }
        finally
        {
            applicationDispatch.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }
    }

    // Runs on the dedicated receive thread of one automatic subscriber connection, which closes
    // the SUB socket after this method returns.
    public void RunFanoutConnectionLoop(
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

    private void RunFanoutReceive(
        string channelName,
        ISubSocket subscriber,
        Action? onActivity,
        Action onProtocolError,
        ZLinkApplicationJobQueue applicationJobQueue,
        ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork> applicationDispatch,
        CancellationToken cancellationToken
    )
    {
        using var receivePoller = ZLinkBackendSocketPoller.Create(subscriber);
        var topicMessagePool = new ZLinkTopicMessageStoragePool();
        TopicMessage? topicMessage = topicMessagePool.Rent();
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                ZLinkApplicationJobQueueLease? admission = null;
                try
                {
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    admission = applicationJobQueue
                        .AcquireAsync(cancellationToken, ZLinkApplicationJobOrigin.Remote)
                        .GetAwaiter()
                        .GetResult();
                    if (!subscriber.Subscribe(topicMessage!, RecvFlags.DontWait))
                        continue;

                    // The beacon shares the publisher's PUB socket, so a subscriber receives
                    // it alongside application records. It is not an application event: it
                    // never reaches the queue, a handler or a message trace.
                    if (ZLinkFanoutLivenessProtocol.IsReservedTopic(topicMessage!.Topic))
                    {
                        if (ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                        {
                            onActivity?.Invoke();
                            var completed = topicMessage;
                            topicMessage = null;
                            topicMessagePool.Return(completed);
                            topicMessage = topicMessagePool.Rent();
                            continue;
                        }

                        onProtocolError();
                        return;
                    }

                    onActivity?.Invoke();
                    var owned = topicMessage!;
                    var envelope = dispatcher.ReadHeader(topicMessage.Parts);
                    if (envelope.Header is not null)
                        admission.MarkQueued();
                    _ = applicationDispatch
                        .PostAsync(
                            new FanoutDispatchWork(
                                channelName,
                                owned,
                                topicMessagePool,
                                admission,
                                envelope
                            ),
                            cancellationToken
                        )
                        .GetAwaiter()
                        .GetResult();
                    // The queue or its rejection callback owns the storage as soon as
                    // PostAsync returns. Clear the local owner before renting the next storage
                    // so a rent failure cannot return the handed-off envelope a second time.
                    topicMessage = null;
                    admission = null;
                    topicMessage = topicMessagePool.Rent();
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
                    admission?.Dispose();
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
