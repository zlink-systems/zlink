using System;
using System.Collections.Generic;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiRouterRouterServer
{
    internal static async Task<int> Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int pollTimeoutMs = ResolveMultiClientPollTimeoutMs(options);
        string endpoint = MultiEndpointFor(options.Transport,
            "multi-router-router", options);

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateRouterSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        server.SetRoutingId(RoutingId.From("SERVER"u8));

        server.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
        // Match the C relay server: configure the message unit before bind,
        // then recalculate the socket policy before advertising READY. The
        // relay loop must not wait for a connection-ready event count because
        // C begins receiving as soon as clients connect.
        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);
        WriteStdoutLine($"READY,{endpoint}");

        var sockets = new[] { (ISocket)server };
        var eventMasks = new[] { SocketPollIn };
        var pendingReplies = new Queue<PendingReply>();
        // Caller-provided Received reused across every recv on the server
        // hot path. The binding overwrites the internal state in place,
        // avoiding the per-recv Received allocation.
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
                && !await FlushPendingRepliesAsync(server, pendingReplies)
                    .ConfigureAwait(false))
            {
                return 2;
            }

            if ((readyMask & PollEventFlags.PollIn) == 0)
                continue;

            if (!TryRecvNoWait(server, receivedBuffer))
            {
                continue;
            }

            if (receivedBuffer.Parts.Count == 1
                && IsStopTokenPayload(receivedBuffer.FirstPart().AsReadOnlySpan()))
            {
                stop = true;
                continue;
            }
            if (!PerfSocketIo.TryMeasurementPayload(receivedBuffer.Parts,
                    out Message bodyMessage))
            {
                continue;
            }

            RoutingId? maybeRoutingId = receivedBuffer.RoutingId;
            if (maybeRoutingId == null)
                return 2;
            bool sent;
            try
            {
                sent = PerfSocketIo.SendMeasurement(server, maybeRoutingId.Value,
                    bodyMessage.AsReadOnlySpan(), SendFlags.DontWait) > 0;
            }
            catch (ZlinkSubmitException ex)
                when (ex.Result == ZlinkSubmitException.ErrorCode.NotConnected
                      || ex.Result == ZlinkSubmitException.ErrorCode.NotFound)
            {
                continue;
            }
            if (!sent)
            {
                Message reply = bodyMessage.Copy();
                pendingReplies.Enqueue(new PendingReply(maybeRoutingId.Value,
                    reply));
            }
        }

        return 0;
    }

    private static async Task<bool> FlushPendingRepliesAsync(IRouterSocket server,
        Queue<PendingReply> pendingReplies)
    {
        if (pendingReplies.Count == 0)
            return true;

        PendingReply pending = pendingReplies.Peek();
        bool sent;
        try
        {
            sent = PerfSocketIo.SendMeasurement(server, pending.RoutingId,
                pending.Message.AsReadOnlySpan(), SendFlags.DontWait) > 0;
        }
        catch (ZlinkSubmitException ex)
            when (ex.Result == ZlinkSubmitException.ErrorCode.NotConnected
                  || ex.Result == ZlinkSubmitException.ErrorCode.NotFound)
        {
            sent = true;
        }
        if (sent)
        {
            pendingReplies.Dequeue();
            pending.Dispose();
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
