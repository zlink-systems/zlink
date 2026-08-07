using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelReceiveLoop(
    ZLinkFanoutPacketDispatcher dispatcher,
    ZLinkClientServerDispatcher clientServerDispatcher)
{
    private static readonly TimeSpan ReceivePollInterval =
        TimeSpan.FromMilliseconds(100);

    public async Task RunClientServerLoopAsync(
        string channelName,
        IZLinkBackendRouterSocket router,
        ZLinkClientServerServerIdentity identity,
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkInboundDispatchBudget inboundDispatchBudget,
        CancellationToken cancellationToken)
    {
        using var receivePoller = router.CreateReceivePoller();
        await using var applicationDispatch =
            new ZLinkChannelApplicationDispatchQueue<ClientServerDispatchWork>(
                $"client-server-application:{channelName}",
                errorSink,
                cancellationToken,
                DispatchClientServerAsync,
                RejectClientServerDispatch);
        var receiveStoragePool = new ZLinkReceivedStoragePool();
        identity.AttachRouter(router);
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                Received? received = null;
                ZLinkInboundReceivePermit? receivePermit = null;
                try
                {
                    identity.TickLiveness(router);
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    // Do not wait for application HWM capacity in the only
                    // loop that also receives ClientServer control frames.
                    // Return to the poller so control progress continues.
                    if (!inboundDispatchBudget.TryAcquireReceive(
                            out receivePermit))
                        continue;
                    received = receiveStoragePool.Rent();
                    if (!router.Recv(received, RecvFlags.DontWait))
                        continue;

                    if (received.RequestSeq is null
                        && received.Parts.Count == 1
                        && received.Parts[0].Size == 0)
                        continue;
                    if (ZLinkClientServerControlProtocol.IsControl(received.Parts))
                    {
                        ReplyClientServerControl(router, received, identity);
                        continue;
                    }
                    if (received.RoutingId is not { } applicationSource
                        || !identity.TryGetAdmittedMaximumMessageBytes(
                            applicationSource,
                            out var admittedMaximumMessageBytes))
                        continue;
                    if (!ZLinkClientServerMessageBound.Fits(
                            received.Parts,
                            admittedMaximumMessageBytes))
                    {
                        clientServerDispatcher.RejectMessageTooLarge(
                            channelName,
                            router,
                            received,
                            applicationDispatch.ReplyGate,
                            admittedMaximumMessageBytes);
                        continue;
                    }
                    var owned = received;
                    var payloadBytes = MeasurePayloadBytes(owned.Parts);
                    var overageReservation = inboundDispatchBudget.Received(
                        receivePermit,
                        payloadBytes);
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                    receivePermit = null;
                    _ = await applicationDispatch.PostAsync(
                            new ClientServerDispatchWork(
                                channelName,
                                router,
                                owned,
                                receiveStoragePool,
                                applicationDispatch.ReplyGate,
                                admittedMaximumMessageBytes),
                            inboundDispatchBudget,
                            payloadBytes,
                            cancellationToken,
                            overageReservation)
                        .ConfigureAwait(false);
                    // PostAsync either handed ownership to the queue or ran
                    // the rejection callback. Keep the storage local until that
                    // result is known so a pre-handoff exception is returned by
                    // this loop's finally block.
                    received = null;
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
                        exception);
                }
                finally
                {
                    if (received is { } storage)
                        receiveStoragePool.Return(storage);
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                }
            }
        }
        finally
        {
            identity.DetachRouter(router);
        }
    }

    private void RejectClientServerDispatch(
        ClientServerDispatchWork work)
    {
        try
        {
            clientServerDispatcher.RejectOverloaded(
                work.ChannelName,
                work.Router,
                work.Received,
                work.ReplyGate,
                work.MaximumMessageBytes);
        }
        finally
        {
            work.ReceiveStoragePool.Return(work.Received);
        }
    }

    private async ValueTask DispatchClientServerAsync(
        ClientServerDispatchWork work,
        CancellationToken cancellationToken)
    {
        try
        {
            await clientServerDispatcher.DispatchAsync(
                    work.ChannelName,
                    work.Router,
                    work.Received,
                    work.ReplyGate,
                    work.MaximumMessageBytes,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            work.ReceiveStoragePool.Return(work.Received);
        }
    }

    private static void ReplyClientServerControl(
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkClientServerServerIdentity identity)
    {
        if (received.RoutingId is not { } sourceRid)
            return;
        if (ZLinkClientServerControlProtocol.TryDecodeLivenessAck(
                received.Parts,
                out var ackId))
        {
            identity.AcceptLivenessAck(sourceRid, ackId);
            return;
        }
        if (ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                received.Parts,
                out var probeId))
        {
            identity.RecordLivenessProbe(sourceRid);
            var ack =
                ZLinkClientServerControlProtocol.EncodeLivenessAck(probeId);
            if (received.RequestSeq is not null)
                ReplyOwned(router, sourceRid, received.RequestSeq, ack);
            else
                SendOwned(router, sourceRid, ack);
            return;
        }
        var snapshot = identity.Read();
        var valid = ZLinkClientServerControlProtocol.TryDecodeHello(
            received.Parts,
            out var hello);
        var accepted = valid
            && hello is not null
            && StringComparer.Ordinal.Equals(
                hello.ChannelName,
                identity.ChannelName)
            && StringComparer.Ordinal.Equals(
                hello.SecurityIdentity,
                identity.SecurityIdentity);
        var negotiatedMaximumMessageBytes = accepted
            ? Math.Min(
                hello!.NormalizedEffectiveMaxMessageBytes,
                identity.NormalizedEffectiveMaxMessageBytes)
            : 0;
        var reply = accepted
            ? ZLinkClientServerControlProtocol.EncodeAdmission(
                identity.ToAdmission(snapshot) with
                {
                    NormalizedEffectiveMaxMessageBytes = negotiatedMaximumMessageBytes
                })
            : ZLinkClientServerControlProtocol.EncodeReject(reason: 1);
        if (ReplyOwned(router, sourceRid, received.RequestSeq, reply)
            && accepted)
            identity.AdmitPeer(sourceRid, negotiatedMaximumMessageBytes);
    }

    private static bool ReplyOwned(
        IZLinkBackendRouterSocket router,
        RoutingId sourceRid,
        ulong? requestSeq,
        Message reply)
    {
        if (requestSeq is not { } value)
        {
            reply.Dispose();
            return false;
        }
        try
        {
            router.Reply(sourceRid, value, reply);
            return true;
        }
        catch
        {
            reply.Dispose();
            throw;
        }
    }

    private static bool SendOwned(
        IZLinkBackendRouterSocket router,
        RoutingId sourceRid,
        Message message)
    {
        try
        {
            if (router.Send(sourceRid, message, SendFlags.DontWait))
                return true;
        }
        catch
        {
        }
        message.Dispose();
        return false;
    }

    public async Task RunSubscriberLoopAsync(
        string channelName,
        IZLinkBackendSubscriberSocket subscriber,
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkInboundDispatchBudget inboundDispatchBudget,
        CancellationToken cancellationToken)
    {
        using var receivePoller = subscriber.CreateReceivePoller();
        await using var applicationDispatch =
            new ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork>(
                $"fanout-application:{channelName}",
                errorSink,
                cancellationToken,
                DispatchFanoutAsync,
                RejectFanoutDispatch);
        var topicMessagePool = new ZLinkTopicMessageStoragePool();
        TopicMessage? topicMessage = topicMessagePool.Rent();
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                ZLinkInboundReceivePermit? receivePermit = null;
                try
                {
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    // The subscriber loop also receives liveness beacons. Do not
                    // block that control path behind application HWM capacity.
                    if (!inboundDispatchBudget.TryAcquireReceive(
                            out receivePermit))
                        continue;
                    if (!subscriber.TryReceive(topicMessage!))
                        continue;

                    // The beacon shares the publisher's PUB socket, so a manual
                    // subscriber receives it alongside application records. It is
                    // not an application event: it never reaches the queue, a
                    // handler or a message trace.
                    if (ZLinkFanoutLivenessProtocol.IsReservedTopic(
                            topicMessage!.Topic))
                    {
                        if (ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                        {
                            var completed = topicMessage;
                            topicMessage = null;
                            topicMessagePool.Return(completed);
                            topicMessage = topicMessagePool.Rent();
                            continue;
                        }

                        // A reserved topic carrying anything else is a protocol
                        // error. Leaving the loop closes this publisher's socket,
                        // which is the only connection it governs.
                        errorSink.ReportRuntimeTaskException(
                            $"channel-subscriber:{channelName}",
                            new InvalidOperationException(
                                "Fanout publisher sent a malformed liveness beacon."));
                        return;
                    }

                    var owned = topicMessage!;
                    var payloadBytes = MeasurePayloadBytes(owned.Parts);
                    var overageReservation = inboundDispatchBudget.Received(
                        receivePermit,
                        payloadBytes);
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                    receivePermit = null;
                    _ = await applicationDispatch.PostAsync(
                            new FanoutDispatchWork(
                                channelName,
                                owned,
                                topicMessagePool),
                            inboundDispatchBudget,
                            payloadBytes,
                            cancellationToken,
                            overageReservation)
                        .ConfigureAwait(false);
                    // The queue or its rejection callback owns the storage as
                    // soon as PostAsync returns. Clear the local owner before
                    // renting the next storage so a rent failure cannot return
                    // the handed-off envelope a second time.
                    topicMessage = null;
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
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                }
            }
        }
        finally
        {
            if (topicMessage is { } storage)
                topicMessagePool.Return(storage);
        }
    }

    public async Task RunFanoutConnectionLoopAsync(
        string channelName,
        IZLinkBackendSubscriberSocket subscriber,
        Action onActivity,
        Action onProtocolError,
        IZLinkRuntimeFailureReporter errorSink,
        ZLinkInboundDispatchBudget inboundDispatchBudget,
        CancellationToken cancellationToken)
    {
        using var receivePoller = subscriber.CreateReceivePoller();
        await using var applicationDispatch =
            new ZLinkChannelApplicationDispatchQueue<FanoutDispatchWork>(
                $"fanout-automatic-application:{channelName}",
                errorSink,
                cancellationToken,
                DispatchFanoutAsync,
                RejectFanoutDispatch);
        var topicMessagePool = new ZLinkTopicMessageStoragePool();
        TopicMessage? topicMessage = topicMessagePool.Rent();
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                ZLinkInboundReceivePermit? receivePermit = null;
                try
                {
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    // Keep liveness processing independent from application
                    // dispatch pressure by returning to the poller immediately.
                    if (!inboundDispatchBudget.TryAcquireReceive(
                            out receivePermit))
                        continue;
                    if (!subscriber.TryReceive(topicMessage!))
                        continue;

                    if (ZLinkFanoutLivenessProtocol.IsReservedTopic(
                            topicMessage!.Topic))
                    {
                        if (ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                        {
                            onActivity();
                            var completed = topicMessage;
                            topicMessage = null;
                            topicMessagePool.Return(completed);
                            topicMessage = topicMessagePool.Rent();
                            continue;
                        }

                        onProtocolError();
                        return;
                    }

                    onActivity();
                    var owned = topicMessage!;
                    var payloadBytes = MeasurePayloadBytes(owned.Parts);
                    var overageReservation = inboundDispatchBudget.Received(
                        receivePermit,
                        payloadBytes);
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                    receivePermit = null;
                    _ = await applicationDispatch.PostAsync(
                            new FanoutDispatchWork(
                                channelName,
                                owned,
                                topicMessagePool),
                            inboundDispatchBudget,
                            payloadBytes,
                            cancellationToken,
                            overageReservation)
                        .ConfigureAwait(false);
                    topicMessage = null;
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
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                }
            }
        }
        finally
        {
            if (topicMessage is { } storage)
                topicMessagePool.Return(storage);
        }
    }

    private static void RejectFanoutDispatch(FanoutDispatchWork work) =>
        work.TopicMessagePool.Return(work.TopicMessage);

    private async ValueTask DispatchFanoutAsync(
        FanoutDispatchWork work,
        CancellationToken cancellationToken)
    {
        try
        {
            await dispatcher.DispatchEventMessageAsync(
                    work.ChannelName,
                    work.TopicMessage,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            work.TopicMessagePool.Return(work.TopicMessage);
        }
    }

    private static ulong MeasurePayloadBytes(IReadOnlyList<Message> parts)
    {
        ulong total = 0;
        foreach (var part in parts)
            total = checked(total + checked((ulong)part.Size));
        return total;
    }

    private static bool IsReadable(ZLinkBackendSocketReadiness readiness) =>
        (readiness & (ZLinkBackendSocketReadiness.Readable
                      | ZLinkBackendSocketReadiness.Error
                      | ZLinkBackendSocketReadiness.Priority)) != 0;

    private readonly record struct ClientServerDispatchWork(
        string ChannelName,
        IZLinkBackendRouterSocket Router,
        Received Received,
        ZLinkReceivedStoragePool ReceiveStoragePool,
        ZLinkChannelReplyGate ReplyGate,
        uint MaximumMessageBytes);

    private readonly record struct FanoutDispatchWork(
        string ChannelName,
        TopicMessage TopicMessage,
        ZLinkTopicMessageStoragePool TopicMessagePool);
}
