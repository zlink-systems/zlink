using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Systems.Zlink;

internal static partial class PerfRunner
{
    internal const PollEventFlags SocketPollIn = PollEventFlags.PollIn;
    internal const PollEventFlags SocketPollOut = PollEventFlags.PollOut;

    internal static bool IsSupportedTransport(string transport)
    {
        return transport.Equals("tcp", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("tls", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("ws", StringComparison.OrdinalIgnoreCase)
            || transport.Equals("wss", StringComparison.OrdinalIgnoreCase);
    }

    internal static bool ParseEndpointArg(string endpoint,
        out string normalizedEndpoint)
    {
        normalizedEndpoint = endpoint?.Trim() ?? string.Empty;
        return !string.IsNullOrWhiteSpace(normalizedEndpoint);
    }

    internal static List<ISocket> WaitClientConnectReadyAll(
        PollManager pollManager, List<ISocket> clients,
        List<MonitorSocket> monitors, int readyTimeoutMs)
    {
        int count = Math.Min(clients.Count, monitors.Count);
        var activeClients = new List<ISocket>(count);
        if (count == 0)
            return activeClients;

        var ready = new bool[count];
        var pendingIndices = new List<int>(count);
        for (int i = 0; i < count; i++)
        {
            if (TryConsumeReadyEvent(monitors[i]))
            {
                ready[i] = true;
                continue;
            }

            pendingIndices.Add(i);
        }

        if (pendingIndices.Count == 0)
        {
            for (int i = 0; i < count; i++)
                if (ready[i])
                    activeClients.Add(clients[i]);
            return activeClients;
        }

        long deadlineTicks = DeadlineTicksFromMilliseconds(readyTimeoutMs);
        while (pendingIndices.Count > 0)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (nowTicks >= deadlineTicks)
                break;

            int pollCount = pendingIndices.Count;
            var pollMonitors = new List<MonitorSocket>(pollCount);
            var pollIndices = new int[pollCount];
            for (int i = 0; i < pollCount; i++)
            {
                pollMonitors.Add(monitors[pendingIndices[i]]);
                pollIndices[i] = i;
            }

            int readyEvents = pollManager.PollMonitors(pollMonitors,
                pollIndices, pollCount, deadlineTicks, nowTicks);
            if (readyEvents <= 0)
                continue;

            for (int i = pollCount - 1; i >= 0; i--)
            {
                int index = pendingIndices[i];
                if (!TryConsumeReadyEvent(monitors[index]))
                    continue;

                ready[index] = true;
                pendingIndices.RemoveAt(i);
            }
        }

        for (int i = 0; i < count; i++)
        {
            if (ready[i])
                activeClients.Add(clients[i]);
        }

        return activeClients;
    }

    internal static int PollSocketReadReady(PollManager pollManager,
        IReadOnlyList<ISocket> sockets, int timeoutMs)
    {
        return pollManager.PollSockets(sockets, PollEventFlags.PollIn, timeoutMs);
    }

    internal static int PollSocketEvents(PollManager pollManager,
        IReadOnlyList<ISocket> sockets, IReadOnlyList<PollEventFlags> eventMasks,
        int timeoutMs)
    {
        return pollManager.PollSockets(sockets, eventMasks, timeoutMs);
    }

    internal static int ReadySocketCount(PollManager pollManager)
    {
        return pollManager.ReadySocketCount;
    }

    internal static int ReadySocketIndexAt(PollManager pollManager, int offset)
    {
        return pollManager.ReadySocketIndexAt(offset);
    }

    internal static PollEventFlags ReadySocketMaskAt(PollManager pollManager,
        int offset)
    {
        return pollManager.ReadySocketMaskAt(offset);
    }

    private static bool TryConsumeReadyEvent(MonitorSocket monitor)
    {
        return DrainReadyEvents(monitor) > 0;
    }

    internal static int DrainReadyEvents(MonitorSocket monitor)
    {
        int readyCount = 0;
        while (true)
        {
            try
            {
                MonitorEvent? evt = monitor.Recv(RecvFlags.DontWait);
                if (evt == null)
                    return readyCount;
                if (IsMonitorReady(evt.Event))
                    readyCount++;
            }
            catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                            || IsInterrupted(ex.NativeErrno))
            {
                return readyCount;
            }
            catch (ObjectDisposedException)
            {
                return readyCount;
            }
        }
    }

    internal static void TrySendStopToken(IReadOnlyList<ISocket> activeClients)
    {
        if (activeClients == null || activeClients.Count == 0)
            return;

        for (int i = 0; i < activeClients.Count; i++)
        {
            try
            {
                if (activeClients[i] is IMessageSocket messageSocket)
                {
                    _ = SendBlocking(messageSocket, MultiStopToken.AsSpan(),
                        SendFlags.None);
                }
            }
            catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                            || IsInterrupted(ex.NativeErrno))
            {
            }
            catch (ObjectDisposedException)
            {
            }
        }
    }

    internal static void DisposeAllQuietly<T>(IEnumerable<T> resources)
        where T : class, IDisposable
    {
        foreach (T resource in resources)
            TryDisposeQuietly(resource);
    }

    internal sealed class RunnerControlState : IDisposable
    {
        private readonly int _msgSize;
        private readonly ManualResetEventSlim _startSignal = new(false);
        private readonly ManualResetEventSlim _controlConnectedSignal = new(false);
        private readonly Thread _readerThread;
        private int _startRequested;
        private int _stopRequested;
        private Action<string>? _connectControlCallback;
        private readonly bool _debug =
            PerfEnv.ReadPositive("PERF_DOTNET_CONTROL_DEBUG", 0) > 0;

        internal RunnerControlState(int msgSize)
        {
            _msgSize = msgSize;

            _readerThread = new Thread(ReadLoop)
            {
                IsBackground = true,
            };
            _readerThread.Start();
        }

        internal bool StopRequested => Volatile.Read(ref _stopRequested) != 0;

        internal void SetConnectControlCallback(Action<string> callback)
        {
            _connectControlCallback = callback;
        }

        internal bool WaitForStart(int timeoutMs)
        {
            if (_startSignal.IsSet)
                return true;

            int boundedTimeoutMs = Math.Max(1, timeoutMs);
            _startSignal.Wait(boundedTimeoutMs);
            return _startSignal.IsSet && !StopRequested;
        }

        internal bool WaitForControlConnected(int timeoutMs)
        {
            if (_controlConnectedSignal.IsSet)
                return true;

            int boundedTimeoutMs = Math.Max(1, timeoutMs);
            _controlConnectedSignal.Wait(boundedTimeoutMs);
            return _controlConnectedSignal.IsSet && !StopRequested;
        }

        public void Dispose()
        {
            Volatile.Write(ref _stopRequested, 1);
            TrySet(_startSignal);
            TrySet(_controlConnectedSignal);
        }

        private void ReadLoop()
        {
            string expectedStart = $"START,{_msgSize}";
            const string connectControlPrefix = "CONNECT_CONTROL,";
            const string controlConnectedPrefix = "CONTROL_CONNECTED,";

            while (true)
            {
                string? line = Console.ReadLine();
                if (line == null)
                {
                    Volatile.Write(ref _stopRequested, 1);
                    TrySet(_startSignal);
                    TrySet(_controlConnectedSignal);
                    return;
                }

                string command = line.Trim();
                if (_debug)
                    Console.Error.WriteLine($"control_debug:stdin:{command}");
                if (string.IsNullOrEmpty(command))
                    continue;

                if (string.Equals(command, "STOP",
                        StringComparison.OrdinalIgnoreCase)
                    || string.Equals(command, "QUIT",
                        StringComparison.OrdinalIgnoreCase))
                {
                    Volatile.Write(ref _stopRequested, 1);
                    TrySet(_startSignal);
                    TrySet(_controlConnectedSignal);
                    return;
                }

                if (command.StartsWith(connectControlPrefix,
                        StringComparison.Ordinal))
                {
                    string ep = command.Substring(connectControlPrefix.Length)
                        .Trim();
                    if (_debug)
                        Console.Error.WriteLine($"control_debug:connect:{ep}");
                    _connectControlCallback?.Invoke(ep);
                    continue;
                }

                if (command.StartsWith(controlConnectedPrefix,
                        StringComparison.Ordinal))
                {
                    TrySet(_controlConnectedSignal);
                    continue;
                }

                if (string.Equals(command, expectedStart,
                        StringComparison.Ordinal))
                {
                    if (_debug)
                        Console.Error.WriteLine($"control_debug:start:{command}");
                    Volatile.Write(ref _startRequested, 1);
                    TrySet(_startSignal);
                }
            }
        }

        private static void TrySet(ManualResetEventSlim signal)
        {
            try
            {
                signal.Set();
            }
            catch (ObjectDisposedException)
            {
            }
        }
    }
}
