using System;
using System.Collections.Generic;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerRouterServer
{

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int clientCount = ResolveMultiClients(options);
        int pollTimeoutMs = ResolveMultiClientPollTimeoutMs(options);
        string endpoint = MultiEndpointFor(options.Transport,
            "multi-dealer-router", options);

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateRouterSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        using var monitor = server.MonitorOpen(SocketEvent.ConnectionReady);

        server.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        WriteStdoutLine($"READY,{endpoint}");

        if (!WaitConnectReadyCount(monitor, clientCount, readyTimeoutMs))
            return 2;

        ApplyAutoHwmMsgUnit(ctx, size);
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);

        var sockets = new[] { (ISocket)server };
        var eventMasks = new[] { SocketPollIn };
        var pendingReplies = new Queue<PendingReply>();
        using var receivedBuffer = Received.Create();

        bool stop = false;
        while (!stop)
        {
            eventMasks[0] = pendingReplies.Count > 0
                ? SocketPollIn | SocketPollOut
                : SocketPollIn;
            int readyCount = PollSocketEvents(pollManager, sockets, eventMasks,
                pollTimeoutMs);
            if (readyCount <= 0)
            {
                continue;
            }

            PollEventFlags readyMask = PollEventFlags.None;
            for (int i = 0; i < readyCount; i++)
            {
                if (ReadySocketIndexAt(pollManager, i) == 0)
                    readyMask |= ReadySocketMaskAt(pollManager, i);
            }

            if ((readyMask & PollEventFlags.PollOut) != 0
                && !FlushPendingReplies(server, pendingReplies))
            {
                return 2;
            }

            if ((readyMask & PollEventFlags.PollIn) == 0)
                continue;

            while (true)
            {
                if (!TryRecvNoWait(server, receivedBuffer))
                    break;

                // Skip Parts.Count: it materializes a
                // MultipartMessageCollection wrapper around the lazy
                // single-part message (one heap alloc per recv).
                Message bodyMessage = receivedBuffer.SinglePartOrThrow();
                ReadOnlySpan<byte> body = bodyMessage.AsReadOnlySpan();
                if (IsStopTokenPayload(body))
                {
                    stop = true;
                    break;
                }

                if (pendingReplies.Count == 0
                    && receivedBuffer.Send().Message(bodyMessage)
                        .Flags(SendFlags.DontWait).Submit())
                {
                    continue;
                }

                RoutingId? maybeRoutingId = receivedBuffer.RoutingId;
                if (maybeRoutingId == null)
                    return 2;
                Message reply = bodyMessage.Copy();
                if (!EnqueueReplyOrSend(server, pendingReplies,
                        maybeRoutingId.Value, reply, tryImmediate: false))
                {
                    reply.Dispose();
                    return 2;
                }
            }
        }

        return 0;
    }

    private static bool EnqueueReplyOrSend(IRouterSocket server,
        Queue<PendingReply> pendingReplies, RoutingId routingId, Message reply,
        bool tryImmediate = true)
    {
        if (tryImmediate && pendingReplies.Count == 0)
        {
            if (server.Send(routingId).Message(reply)
                    .Flags(SendFlags.DontWait).Submit())
            {
                reply.Dispose();
                return true;
            }
        }

        pendingReplies.Enqueue(new PendingReply(routingId, reply));
        return true;
    }

    private static bool FlushPendingReplies(IRouterSocket server,
        Queue<PendingReply> pendingReplies)
    {
        while (pendingReplies.Count > 0)
        {
            PendingReply pending = pendingReplies.Peek();
            if (server.Send(pending.RoutingId).Message(pending.Message)
                    .Flags(SendFlags.DontWait).Submit())
            {
                pendingReplies.Dequeue();
                pending.Dispose();
                continue;
            }

            return true;
        }

        return true;
    }

    private static bool TryRecvNoWait(IRouterSocket socket, Received result)
    {
        return socket.Recv(result, RecvFlags.DontWait);
    }

    private sealed class PendingReply : IDisposable
    {
        internal PendingReply(RoutingId routingId, Message message)
        {
            RoutingId = routingId;
            Message = message;
        }

        internal RoutingId RoutingId { get; }
        internal Message Message { get; }

        public void Dispose()
        {
            Message.Dispose();
        }
    }
}
