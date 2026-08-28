using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiRoutedRelayServer
{
    internal static async Task<int> RunAsync(IRouterSocket server,
        PollManager pollManager, int pollTimeoutMs, int drainTimeoutMs)
    {
        var sockets = new[] { (ISocket)server };
        var eventMasks = new[] { SocketPollIn };
        var replies = new List<Task<bool>>();
        var state = new RelayState();
        using var receivedBuffer = Received.Create();

        bool stop = false;
        bool success = true;
        int failureObservationPollMs = pollTimeoutMs < 0
            ? 200
            : pollTimeoutMs;
        while (!stop && success && !state.Failed)
        {
            if (!await RemoveCompletedRepliesAsync(replies).ConfigureAwait(false))
            {
                success = false;
                break;
            }

            int readyCount = PollSocketEvents(pollManager, sockets,
                eventMasks, failureObservationPollMs);
            if (readyCount <= 0)
                continue;

            PollEventFlags readyMask = PollEventFlags.None;
            for (int i = 0; i < readyCount; i++)
            {
                if (ReadySocketIndexAt(pollManager, i) == 0)
                    readyMask |= ReadySocketMaskAt(pollManager, i);
            }
            if ((readyMask & PollEventFlags.PollIn) == 0)
                continue;

            while (!state.Failed && TryRecvNoWait(server, receivedBuffer))
            {
                if (receivedBuffer.Parts.Count == 1
                    && IsStopTokenPayload(receivedBuffer.FirstPart()
                        .AsReadOnlySpan()))
                {
                    stop = true;
                    break;
                }
                if (!PerfSocketIo.TryMeasurementPayload(
                        receivedBuffer.Parts, out Message bodyMessage))
                {
                    continue;
                }

                RoutingId? maybeRoutingId = receivedBuffer.RoutingId;
                if (maybeRoutingId == null)
                {
                    success = false;
                    break;
                }

                replies.Add(SendReplyAsync(server, maybeRoutingId.Value,
                    bodyMessage.Copy(), state));
            }
        }

        if (replies.Count > 0)
        {
            Task<bool[]> drain = Task.WhenAll(replies);
            Task completed = await Task.WhenAny(drain,
                Task.Delay(Math.Max(1, drainTimeoutMs))).ConfigureAwait(false);
            if (!ReferenceEquals(completed, drain))
            {
                DebugFailure("async reply drain timed out", null);
                return 2;
            }
            foreach (bool replySuccess in await drain.ConfigureAwait(false))
                success &= replySuccess;
        }
        return success && !state.Failed ? 0 : 2;
    }

    private static async Task<bool> SendReplyAsync(IRouterSocket server,
        RoutingId routingId, Message message, RelayState state)
    {
        try
        {
            await PerfSocketIo.SendMeasurementAsync(server, routingId, message)
                .ConfigureAwait(false);
            return true;
        }
        catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))
        {
            // The source disconnected after its request was received.
            return true;
        }
        catch (Exception ex)
        {
            DebugFailure("async reply send", ex);
            state.Fail();
            return false;
        }
        finally
        {
            message.Dispose();
        }
    }

    private static async Task<bool> RemoveCompletedRepliesAsync(
        List<Task<bool>> replies)
    {
        bool result = true;
        for (int i = replies.Count - 1; i >= 0; --i)
        {
            Task<bool> reply = replies[i];
            if (!reply.IsCompleted)
                continue;
            replies.RemoveAt(i);
            result &= await reply.ConfigureAwait(false);
        }
        return result;
    }

    private static bool TryRecvNoWait(IRouterSocket socket, Received result)
    {
        return socket.Recv(result, RecvFlags.DontWait);
    }

    private static bool IsStaleRoute(ZlinkSubmitException error)
    {
        return error.Result == ZlinkSubmitException.ErrorCode.NotConnected
            || error.Result == ZlinkSubmitException.ErrorCode.NotFound;
    }

    private static void DebugFailure(string operation, Exception? error)
    {
        if (Environment.GetEnvironmentVariable("PERF_DEBUG") == null)
            return;
        if (error is ZlinkException zlinkError)
            Console.Error.WriteLine($"MULTI_ROUTED_RELAY {operation} failed "
                + $"errno={zlinkError.NativeErrno}: {error}");
        else if (error != null)
            Console.Error.WriteLine(
                $"MULTI_ROUTED_RELAY {operation} failed: {error}");
        else
            Console.Error.WriteLine(
                $"MULTI_ROUTED_RELAY {operation} failed");
    }

    private sealed class RelayState
    {
        private int _failed;

        internal bool Failed => Volatile.Read(ref _failed) != 0;

        internal void Fail()
        {
            Interlocked.Exchange(ref _failed, 1);
        }
    }
}
