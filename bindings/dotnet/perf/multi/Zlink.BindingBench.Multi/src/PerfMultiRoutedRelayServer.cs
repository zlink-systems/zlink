using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiRoutedRelayServer
{
    private const int ReplyReapInterval = 64;

    internal static async Task<int> RunAsync(IRouterSocket server,
        PollManager pollManager, int pollTimeoutMs, int drainTimeoutMs)
    {
        var sockets = new[] { (ISocket)server };
        var eventMasks = new[] { SocketPollIn };
        var replies = new List<Task>();
        using var receivedBuffer = Received.Create();

        // PERF_POLICY.md:138-143 - the handshake contract is the C one: the
        // runner stops every multi server with a stdin STOP/QUIT line, and the
        // C relay server shuts down gracefully from a stdin watcher
        // (bindings/c/perf/multi/common/perf_multi_relay_server.hpp:667-677).
        // Without this watcher the routed one-way (SENDSEND) server had no
        // stop path and was killed by SIGTERM instead.
        int stopRequested = 0;
        Thread stdinThread = new(() =>
        {
            string? line;
            while ((line = Console.In.ReadLine()) != null)
            {
                if (line == "STOP" || line == "QUIT")
                    break;
            }
            Volatile.Write(ref stopRequested, 1);
        })
        {
            IsBackground = true,
            Name = "multi routed relay server control"
        };
        stdinThread.Start();

        bool stop = false;
        bool success = true;
        int relayedSinceReap = 0;
        // The stdin watcher cannot wake a socket poll that waits forever, so
        // the poll stays bounded and STOP is observed on the next turn. Same
        // reason as the C relay server's auxiliary poll wait.
        int failureObservationPollMs = pollTimeoutMs < 0
            ? 200
            : pollTimeoutMs;
        while (!stop && success && Volatile.Read(ref stopRequested) == 0)
        {
            if (!RemoveCompletedReplies(replies))
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

            while (success && TryRecvNoWait(server, receivedBuffer))
            {
                IReadOnlyList<Message> parts = receivedBuffer.Parts;
                if (parts.Count == 1
                    && IsStopTokenPayload(receivedBuffer.FirstPart()
                        .AsReadOnlySpan()))
                {
                    stop = true;
                    break;
                }
                if (!PerfSocketIo.TryMeasurementPayload(parts, out _))
                {
                    continue;
                }

                if (!TrySubmitReply(receivedBuffer, parts, replies))
                {
                    success = false;
                    break;
                }

                relayedSinceReap++;
                if (relayedSinceReap < ReplyReapInterval)
                    continue;

                relayedSinceReap = 0;
                if (!RemoveCompletedReplies(replies))
                    success = false;
            }
        }

        if (replies.Count > 0)
        {
            Task drain = Task.WhenAll(replies);
            Task completed = await Task.WhenAny(drain,
                Task.Delay(Math.Max(1, drainTimeoutMs))).ConfigureAwait(false);
            if (!ReferenceEquals(completed, drain))
            {
                DebugFailure("async reply drain timed out", null);
                return 2;
            }

            try
            {
                await drain.ConfigureAwait(false);
            }
            catch
            {
                // Observe each terminal below so stale routes remain non-fatal
                // while every other completion failure is reported precisely.
            }
            success &= RemoveCompletedReplies(replies);
        }
        return success ? 0 : 2;
    }

    private static bool TrySubmitReply(Received received,
        IReadOnlyList<Message> parts, List<Task> replies)
    {
        Task reply;
        try
        {
            // Transfer the original routed envelope parts to Core. Async()
            // consumes the parts before returning on a successful submit, so
            // the same Received can immediately be reused by the next Recv.
            reply = received.Send().Messages(parts).Async();
        }
        catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))
        {
            // The source disconnected after its request was received.
            return true;
        }
        catch (Exception ex)
        {
            DebugFailure("async reply send", ex);
            return false;
        }

        if (reply.IsCompletedSuccessfully)
            return true;
        if (reply.IsCompleted)
            return ObserveCompletedReply(reply);

        replies.Add(reply);
        return true;
    }

    private static bool RemoveCompletedReplies(List<Task> replies)
    {
        bool result = true;
        int retained = 0;
        int count = replies.Count;
        for (int i = 0; i < count; i++)
        {
            Task reply = replies[i];
            if (reply.IsCompleted)
            {
                result &= ObserveCompletedReply(reply);
                continue;
            }

            if (retained != i)
                replies[retained] = reply;
            retained++;
        }

        if (retained < count)
            replies.RemoveRange(retained, count - retained);
        return result;
    }

    private static bool ObserveCompletedReply(Task reply)
    {
        try
        {
            reply.GetAwaiter().GetResult();
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
            return false;
        }
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

    // Read once per process: PERF_DEBUG is a launch-time knob and this guard is
    // reached from the relay path, so a per-call lookup would mix harness
    // instrumentation into the measurement.
    private static readonly bool DebugEnabled =
        Environment.GetEnvironmentVariable("PERF_DEBUG") != null;

    private static void DebugFailure(string operation, Exception? error)
    {
        if (!DebugEnabled)
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

}
