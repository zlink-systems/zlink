using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkChannelReceiveLoop(
    ZLinkFanoutPacketDispatcher dispatcher,
    ZLinkClientServerDispatcher clientServerDispatcher)
{
    private static readonly TimeSpan ReceivePollInterval =
        TimeSpan.FromMilliseconds(100);

    public async Task RunClientServerLoopAsync(
        string channelName,
        IRouterSocket router,
        ZLinkClientServerServerIdentity identity,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken)
    {
        using var receivePoller = ZLinkBackendSocketPoller.Create(router);
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
                ZLinkApplicationJobQueueLease? admission = null;
                try
                {
                    await identity.TickLivenessAsync(router, cancellationToken)
                        .ConfigureAwait(false);
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    admission = await applicationJobQueue
                        .AcquireAsync(cancellationToken)
                        .ConfigureAwait(false);
                    received = receiveStoragePool.Rent();
                    if (!router.Recv(received, RecvFlags.DontWait))
                        continue;

                    if (received.RequestSeq is null
                        && received.Parts.Count == 1
                        && received.Parts[0].Size == 0)
                        continue;
                    if (ZLinkClientServerControlProtocol.IsControl(received.Parts))
                    {
                        await ReplyClientServerControlAsync(
                                router,
                                received,
                                identity,
                                cancellationToken)
                            .ConfigureAwait(false);
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
                    if (IsClientServerApplicationRecord(
                            channelName,
                            received.Parts))
                        admission.MarkQueued();
                    _ = await applicationDispatch.PostAsync(
                            new ClientServerDispatchWork(
                                channelName,
                                router,
                                owned,
                                receiveStoragePool,
                                applicationDispatch.ReplyGate,
                                admittedMaximumMessageBytes,
                                admission),
                            cancellationToken)
                        .ConfigureAwait(false);
                    // PostAsync either handed ownership to the queue or ran
                    // the rejection callback. Keep the storage local until that
                    // result is known so a pre-handoff exception is returned by
                    // this loop's finally block.
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
                        exception);
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
            identity.DetachRouter(router);
        }
    }

    private void RejectClientServerDispatch(
        ClientServerDispatchWork work)
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
        CancellationToken cancellationToken)
    {
        try
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(
                work.Admission);
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

    private static async ValueTask ReplyClientServerControlAsync(
        IRouterSocket router,
        Received received,
        ZLinkClientServerServerIdentity identity,
        CancellationToken cancellationToken)
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
                await SendOwnedAsync(
                        router,
                        sourceRid,
                        ack,
                        cancellationToken)
                    .ConfigureAwait(false);
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
                identity.ChannelName.Value)
            && ZLinkClientServerControlProtocol.SecurityIdentityMatches(
                identity.SecurityIdentity,
                hello.SecurityIdentity);
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
        IRouterSocket router,
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
            router.Reply(sourceRid, value)
                .Message(reply)
                .Submit();
            return true;
        }
        catch
        {
            reply.Dispose();
            throw;
        }
    }

    private static async ValueTask<bool> SendOwnedAsync(
        IRouterSocket router,
        RoutingId sourceRid,
        Message message,
        CancellationToken cancellationToken)
    {
        try
        {
            await router.Send(sourceRid)
                .Message(message)
                .Async(cancellationToken)
                .ConfigureAwait(false);
            return true;
        }
        catch
        {
            return false;
        }
        finally
        {
            message.Dispose();
        }
    }

    public async Task RunSubscriberLoopAsync(
        string channelName,
        ISubSocket subscriber,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken)
    {
        using var receivePoller = ZLinkBackendSocketPoller.Create(subscriber);
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
                ZLinkApplicationJobQueueLease? admission = null;
                try
                {
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    admission = await applicationJobQueue
                        .AcquireAsync(cancellationToken)
                        .ConfigureAwait(false);
                    if (!subscriber.Subscribe(
                            topicMessage!, RecvFlags.DontWait))
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
                    if (IsFanoutApplicationRecord(topicMessage.Parts))
                        admission.MarkQueued();
                    _ = await applicationDispatch.PostAsync(
                            new FanoutDispatchWork(
                                channelName,
                                owned,
                                topicMessagePool,
                                admission),
                            cancellationToken)
                        .ConfigureAwait(false);
                    // The queue or its rejection callback owns the storage as
                    // soon as PostAsync returns. Clear the local owner before
                    // renting the next storage so a rent failure cannot return
                    // the handed-off envelope a second time.
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

    public async Task RunFanoutConnectionLoopAsync(
        string channelName,
        ISubSocket subscriber,
        Action onActivity,
        Action onProtocolError,
        ZLinkApplicationJobQueue applicationJobQueue,
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken cancellationToken)
    {
        using var receivePoller = ZLinkBackendSocketPoller.Create(subscriber);
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
                ZLinkApplicationJobQueueLease? admission = null;
                try
                {
                    if (!IsReadable(receivePoller.Wait(ReceivePollInterval)))
                        continue;
                    admission = await applicationJobQueue
                        .AcquireAsync(cancellationToken)
                        .ConfigureAwait(false);
                    if (!subscriber.Subscribe(
                            topicMessage!, RecvFlags.DontWait))
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
                    if (IsFanoutApplicationRecord(topicMessage.Parts))
                        admission.MarkQueued();
                    _ = await applicationDispatch.PostAsync(
                            new FanoutDispatchWork(
                                channelName,
                                owned,
                                topicMessagePool,
                                admission),
                            cancellationToken)
                        .ConfigureAwait(false);
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
        CancellationToken cancellationToken)
    {
        try
        {
            using var invocation = ZLinkApplicationJobQueueInvocation.Enter(
                work.Admission);
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

    private static bool IsClientServerApplicationRecord(
        string channelName,
        IReadOnlyList<Message> parts)
    {
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts, validateFlow: false);
            return StringComparer.Ordinal.Equals(header.ChannelName, channelName)
                   && header.Kind is ZLinkMessageKind.Command
                       or ZLinkMessageKind.Request;
        }
        catch (ZLinkEnvelopeProtocolException)
        {
            return false;
        }
    }

    private static bool IsFanoutApplicationRecord(
        IReadOnlyList<Message> parts)
    {
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(parts, validateFlow: false);
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
            return true;
        }
        catch (ZLinkEnvelopeProtocolException)
        {
            return false;
        }
    }

    private static bool IsReadable(ZLinkBackendSocketReadiness readiness) =>
        (readiness & (ZLinkBackendSocketReadiness.Readable
                      | ZLinkBackendSocketReadiness.Error
                      | ZLinkBackendSocketReadiness.Priority)) != 0;

    private readonly record struct ClientServerDispatchWork(
        string ChannelName,
        IRouterSocket Router,
        Received Received,
        ZLinkReceivedStoragePool ReceiveStoragePool,
        ZLinkChannelReplyGate ReplyGate,
        uint MaximumMessageBytes,
        ZLinkApplicationJobQueueLease Admission);

    private readonly record struct FanoutDispatchWork(
        string ChannelName,
        TopicMessage TopicMessage,
        ZLinkTopicMessageStoragePool TopicMessagePool,
        ZLinkApplicationJobQueueLease Admission);
}
