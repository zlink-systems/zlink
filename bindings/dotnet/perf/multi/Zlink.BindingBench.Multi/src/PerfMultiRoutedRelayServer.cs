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
        var replySender = new PendingReplySender();
        Received receivedBuffer = Received.Create();

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
        // The stdin watcher cannot wake a socket poll that waits forever, so
        // the poll stays bounded and STOP is observed on the next turn. Same
        // reason as the C relay server's auxiliary poll wait.
        int failureObservationPollMs = pollTimeoutMs < 0
            ? 200
            : pollTimeoutMs;
        try
        {
            while (!stop && success && Volatile.Read(ref stopRequested) == 0)
            {
                if (replySender.Completion.IsCompleted)
                {
                    success = await replySender.Completion.ConfigureAwait(false);
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
                        continue;

                    // Move the whole routed envelope into the application FIFO.
                    // Its sender waits for the preceding admission before it
                    // submits this reply, while receive continues with fresh
                    // storage. This is the C relay's immutable pending snapshot.
                    if (!replySender.Enqueue(receivedBuffer))
                    {
                        success = false;
                        break;
                    }
                    receivedBuffer = Received.Create();
                }
            }

            replySender.Complete();
            if (success)
            {
                Task completed = await Task.WhenAny(replySender.Completion,
                    Task.Delay(Math.Max(1, drainTimeoutMs)))
                    .ConfigureAwait(false);
                if (!ReferenceEquals(completed, replySender.Completion))
                {
                    DebugFailure("async reply drain timed out", null);
                    return 2;
                }
                success = await replySender.Completion.ConfigureAwait(false);
            }
            return success ? 0 : 2;
        }
        finally
        {
            replySender.Abort();
            receivedBuffer.Dispose();
        }
    }

    private sealed class PendingReplySender
    {
        private readonly TaskCompletionSource<bool> _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly Queue<Received> _pending = new();
        private readonly object _sync = new();
        private bool _accepting = true;
        private bool _pumping;

        internal Task<bool> Completion => _completion.Task;

        internal bool Enqueue(Received received)
        {
            bool startPump = false;
            lock (_sync)
            {
                if (!_accepting)
                    return false;
                _pending.Enqueue(received);
                if (!_pumping)
                {
                    _pumping = true;
                    startPump = true;
                }
            }
            if (startPump)
                Pump();
            return true;
        }

        internal void Complete()
        {
            bool finished;
            lock (_sync)
            {
                _accepting = false;
                finished = !_pumping && _pending.Count == 0;
            }
            if (finished)
                _completion.TrySetResult(true);
        }

        internal void Abort()
        {
            if (!_completion.Task.IsCompleted)
                FailAndDisposePending();
        }

        private void Pump()
        {
            while (true)
            {
                Received? received;
                bool finished;
                lock (_sync)
                {
                    if (_pending.Count == 0)
                    {
                        _pumping = false;
                        finished = !_accepting;
                        received = null;
                    }
                    else
                    {
                        finished = false;
                        received = _pending.Dequeue();
                    }
                }

                if (received == null)
                {
                    if (finished)
                        _completion.TrySetResult(true);
                    return;
                }

                Task reply;
                try
                {
                    reply = received.Send().Messages(received.Parts).Async();
                }
                catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))
                {
                    received.Dispose();
                    continue;
                }
                catch (Exception ex)
                {
                    received.Dispose();
                    DebugFailure("async reply send", ex);
                    FailAndDisposePending();
                    return;
                }

                if (reply.IsCompleted)
                {
                    bool succeeded = ObserveCompletedReply(reply);
                    received.Dispose();
                    if (succeeded)
                        continue;
                    FailAndDisposePending();
                    return;
                }

                reply.ConfigureAwait(false).GetAwaiter().UnsafeOnCompleted(
                    () => ResumeAfterAdmission(reply, received));
                return;
            }
        }

        private void ResumeAfterAdmission(Task reply, Received received)
        {
            bool succeeded = ObserveCompletedReply(reply);
            received.Dispose();
            if (!succeeded)
            {
                FailAndDisposePending();
                return;
            }
            Pump();
        }

        private void FailAndDisposePending()
        {
            Received[] discarded;
            lock (_sync)
            {
                _accepting = false;
                _pumping = false;
                discarded = _pending.ToArray();
                _pending.Clear();
            }
            foreach (Received received in discarded)
                received.Dispose();
            _completion.TrySetResult(false);
        }
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
