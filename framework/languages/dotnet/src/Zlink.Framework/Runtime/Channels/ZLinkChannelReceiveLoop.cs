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
            new ZLinkChannelApplicationDispatchQueue(
                $"client-server-application:{channelName}",
                errorSink,
                cancellationToken);
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
                    received = router.Recv(RecvFlags.DontWait);
                    if (received is null)
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
                    var owned = received;
                    received = null;
                    var payloadBytes = MeasurePayloadBytes(owned.Parts);
                    var overageReservation = inboundDispatchBudget.Received(
                        receivePermit,
                        payloadBytes);
                    inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                    receivePermit = null;
                    _ = await applicationDispatch.PostAsync(
                            token => DispatchClientServerAsync(
                                channelName,
                                router,
                                owned,
                                applicationDispatch.ReplyGate,
                                token),
                            () => RejectClientServerDispatch(
                                channelName,
                                router,
                                owned,
                                applicationDispatch.ReplyGate),
                            inboundDispatchBudget,
                            payloadBytes,
                            cancellationToken,
                            overageReservation)
                        .ConfigureAwait(false);
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
                    received?.Dispose();
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
        string channelName,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate)
    {
        using (received)
            clientServerDispatcher.RejectOverloaded(
                channelName,
                router,
                received,
                replyGate);
    }

    private async ValueTask DispatchClientServerAsync(
        string channelName,
        IZLinkBackendRouterSocket router,
        Received received,
        ZLinkChannelReplyGate replyGate,
        CancellationToken cancellationToken)
    {
        using (received)
            await clientServerDispatcher.DispatchAsync(
                    channelName,
                    router,
                    received,
                    replyGate,
                    cancellationToken)
                .ConfigureAwait(false);
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
        var reply = accepted
            ? ZLinkClientServerControlProtocol.EncodeAdmission(
                identity.ToAdmission(snapshot) with
                {
                    NormalizedEffectiveMaxMessageBytes = Math.Min(
                        hello!.NormalizedEffectiveMaxMessageBytes,
                        identity.NormalizedEffectiveMaxMessageBytes)
                })
            : ZLinkClientServerControlProtocol.EncodeReject(reason: 1);
        if (ReplyOwned(router, sourceRid, received.RequestSeq, reply)
            && accepted)
            identity.AdmitPeer(sourceRid);
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
            new ZLinkChannelApplicationDispatchQueue(
                $"fanout-application:{channelName}",
                errorSink,
                cancellationToken);
        while (!cancellationToken.IsCancellationRequested)
        {
            TopicMessage? topicMessage = new();
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
                if (!subscriber.Subscribe(topicMessage, RecvFlags.DontWait))
                    continue;

                //  The beacon shares the publisher's PUB socket, so a manual
                //  subscriber receives it alongside application records. It is
                //  not an application event: it never reaches the queue, a
                //  handler or a message trace.
                if (ZLinkFanoutLivenessProtocol.IsReservedTopic(
                        topicMessage.Topic))
                {
                    if (ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                        continue;

                    //  A reserved topic carrying anything else is a protocol
                    //  error. Leaving the loop closes this publisher's socket,
                    //  which is the only connection it governs.
                    errorSink.ReportRuntimeTaskException(
                        $"channel-subscriber:{channelName}",
                        new InvalidOperationException(
                            "Fanout publisher sent a malformed liveness beacon."));
                    return;
                }

                var owned = topicMessage;
                topicMessage = null;
                var payloadBytes = MeasurePayloadBytes(owned.Parts);
                var overageReservation = inboundDispatchBudget.Received(
                    receivePermit,
                    payloadBytes);
                inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                receivePermit = null;
                _ = await applicationDispatch.PostAsync(
                        token => DispatchFanoutAsync(channelName, owned, token),
                        owned.Dispose,
                        inboundDispatchBudget,
                        payloadBytes,
                        cancellationToken,
                        overageReservation)
                    .ConfigureAwait(false);
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
                topicMessage?.Dispose();
                inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
            }
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
            new ZLinkChannelApplicationDispatchQueue(
                $"fanout-automatic-application:{channelName}",
                errorSink,
                cancellationToken);
        while (!cancellationToken.IsCancellationRequested)
        {
            TopicMessage? topicMessage = new();
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
                if (!subscriber.Subscribe(topicMessage, RecvFlags.DontWait))
                    continue;

                if (ZLinkFanoutLivenessProtocol.IsReservedTopic(
                        topicMessage.Topic))
                {
                    if (ZLinkFanoutLivenessProtocol.IsValidBeacon(topicMessage))
                    {
                        onActivity();
                        continue;
                    }

                    onProtocolError();
                    return;
                }

                onActivity();
                var owned = topicMessage;
                topicMessage = null;
                var payloadBytes = MeasurePayloadBytes(owned.Parts);
                var overageReservation = inboundDispatchBudget.Received(
                    receivePermit,
                    payloadBytes);
                inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
                receivePermit = null;
                _ = await applicationDispatch.PostAsync(
                        token => DispatchFanoutAsync(channelName, owned, token),
                        owned.Dispose,
                        inboundDispatchBudget,
                        payloadBytes,
                        cancellationToken,
                        overageReservation)
                    .ConfigureAwait(false);
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
                topicMessage?.Dispose();
                inboundDispatchBudget.CompleteReceiveAttempt(receivePermit);
            }
        }
    }

    private async ValueTask DispatchFanoutAsync(
        string channelName,
        TopicMessage topicMessage,
        CancellationToken cancellationToken)
    {
        using (topicMessage)
            await dispatcher.DispatchEventMessageAsync(
                    channelName,
                    topicMessage,
                    cancellationToken)
                .ConfigureAwait(false);
    }

    private static ulong MeasurePayloadBytes(IReadOnlyList<Message> parts)
    {
        ulong total = 0;
        foreach (var part in parts)
            total = checked(total + checked((ulong)part.Size));
        return total;
    }

    private static bool IsReadable(PollEventFlags readiness) =>
        (readiness & (PollEventFlags.PollIn
                      | PollEventFlags.PollErr
                      | PollEventFlags.PollPri)) != 0;
}
