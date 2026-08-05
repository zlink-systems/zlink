using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;

public sealed class MonitorReadyPoller : IDisposable
{
    private readonly List<ISocketMonitor> _activeMonitors = new();

    public int Poll(List<ISocketMonitor> monitors, int[] activeIndices,
        int activeCount, long deadlineTicks, long nowTicks)
    {
        if (activeCount <= 0)
            return 0;

        long remainingTicks = deadlineTicks - nowTicks;
        if (remainingTicks <= 0)
            return 0;

        long remainingMs = (remainingTicks * 1000L + Stopwatch.Frequency - 1)
            / Stopwatch.Frequency;
        int timeoutMs = remainingMs > int.MaxValue
            ? int.MaxValue
            : (int)remainingMs;

        _activeMonitors.Clear();
        for (int i = 0; i < activeCount; i++)
            _activeMonitors.Add(monitors[activeIndices[i]]);

        try
        {
            return ZlinkPoll.Poll(_activeMonitors, timeoutMs);
        }
        catch (ZlinkException ex) when (PerfShared.IsWouldBlock(ex.NativeErrno)
                                        || PerfShared.IsInterrupted(ex.NativeErrno)
                                        || ex.NativeErrno == 0)
        {
            return 0;
        }
    }

    public void Dispose()
    {
        _activeMonitors.Clear();
    }
}

public sealed class SocketReadyPoller : IDisposable
{
    private PollEvent[] _readyEvents = Array.Empty<PollEvent>();
    private int[] _readyIndexes = Array.Empty<int>();
    private PollEventFlags[] _readyMasks = Array.Empty<PollEventFlags>();
    private IPoller? _poller;
    private ISocket[] _registeredSockets = Array.Empty<ISocket>();
    private PollEventFlags[] _registeredMasks = Array.Empty<PollEventFlags>();
    private PollEventFlags[] _requestedMasks = Array.Empty<PollEventFlags>();
    private int _activePollerSize;
    private int _readyCount;

    public int Poll(IReadOnlyList<ISocket> sockets,
        IReadOnlyList<PollEventFlags> eventMasks, int timeoutMs)
    {
        int count = sockets.Count;
        if (count <= 0 || eventMasks.Count < count)
            return 0;

        EnsureCapacity(count);
        _readyCount = 0;
        EnsurePollerState(sockets, eventMasks, count);

        try
        {
            if (_poller == null || _activePollerSize == 0)
                return 0;

            int written = _poller.Wait(_readyEvents.AsSpan(0, count),
                TimeSpan.FromMilliseconds(timeoutMs));
            if (written == 0)
                return 0;

            int ready = 0;
            for (int i = 0; i < written; i++)
            {
                PollEvent readyEvent = _readyEvents[i];
                nuint slot = readyEvent.Slot;
                if (slot > (nuint)int.MaxValue)
                    continue;
                int index = (int)slot;
                PollEventFlags revents = readyEvent.Revents;
                if ((uint)index >= (uint)count
                    || _registeredMasks[index] == PollEventFlags.None)
                    continue;
                if (revents == PollEventFlags.None)
                    continue;

                _readyIndexes[ready] = index;
                _readyMasks[ready] = revents;
                ready++;
            }

            _readyCount = ready;
            return ready;
        }
        catch (ZlinkException ex) when (PerfShared.IsWouldBlock(ex.NativeErrno)
                                        || PerfShared.IsInterrupted(ex.NativeErrno)
                                        || ex.NativeErrno == 0)
        {
            return 0;
        }
    }

    public int Poll(IReadOnlyList<ISocket> sockets, PollEventFlags events,
        int timeoutMs)
    {
        int count = sockets.Count;
        if (count <= 0)
            return 0;

        EnsureCapacity(count);
        for (int i = 0; i < count; i++)
            _requestedMasks[i] = events;
        return Poll(sockets, _requestedMasks, timeoutMs);
    }

    public int ReadyCount => _readyCount;

    public int ReadyIndexAt(int offset)
    {
        if ((uint)offset >= (uint)_readyCount)
            throw new ArgumentOutOfRangeException(nameof(offset));
        return _readyIndexes[offset];
    }

    public PollEventFlags ReadyMaskAt(int offset)
    {
        if ((uint)offset >= (uint)_readyCount)
            throw new ArgumentOutOfRangeException(nameof(offset));
        return _readyMasks[offset];
    }

    public void Dispose()
    {
        _poller?.Dispose();
        _poller = null;
    }

    private void EnsureCapacity(int count)
    {
        if (_readyEvents.Length < count)
            _readyEvents = new PollEvent[count];
        if (_readyIndexes.Length < count)
            _readyIndexes = new int[count];
        if (_readyMasks.Length < count)
            _readyMasks = new PollEventFlags[count];
        if (_registeredSockets.Length < count)
            _registeredSockets = new ISocket[count];
        if (_registeredMasks.Length < count)
            _registeredMasks = new PollEventFlags[count];
        if (_requestedMasks.Length < count)
            _requestedMasks = new PollEventFlags[count];
    }

    private void EnsurePollerState(IReadOnlyList<ISocket> sockets,
        IReadOnlyList<PollEventFlags> eventMasks, int count)
    {
        if (_poller == null || !HasSameLayout(sockets, count))
        {
            RebuildPoller(sockets, eventMasks, count);
            return;
        }

        for (int i = 0; i < count; i++)
        {
            PollEventFlags previous = _registeredMasks[i];
            PollEventFlags current = eventMasks[i];
            if (previous == current)
                continue;

            if (previous == PollEventFlags.None)
            {
                if (current != PollEventFlags.None)
                {
                    _poller.Add(sockets[i], current, (nuint)i);
                    _activePollerSize++;
                }
            }
            else if (current == PollEventFlags.None)
            {
                if (_poller.Remove(sockets[i]))
                    _activePollerSize--;
            }
            else
            {
                _poller.Modify(sockets[i], current);
            }

            _registeredMasks[i] = current;
        }
    }

    private bool HasSameLayout(IReadOnlyList<ISocket> sockets, int count)
    {
        for (int i = 0; i < count; i++)
        {
            if (!ReferenceEquals(_registeredSockets[i], sockets[i]))
                return false;
        }

        return true;
    }

    private void RebuildPoller(IReadOnlyList<ISocket> sockets,
        IReadOnlyList<PollEventFlags> eventMasks, int count)
    {
        _poller?.Dispose();
        _poller = Zlink.CreatePoller();
        _activePollerSize = 0;
        _readyCount = 0;

        for (int i = 0; i < count; i++)
        {
            _registeredSockets[i] = sockets[i];
            PollEventFlags mask = eventMasks[i];
            _registeredMasks[i] = mask;
            if (mask != PollEventFlags.None)
            {
                _poller.Add(sockets[i], mask, (nuint)i);
                _activePollerSize++;
            }
        }
    }
}

public sealed class PollManager : IDisposable
{
    private readonly MonitorReadyPoller _monitorPoller = new();
    private readonly SocketReadyPoller _socketPoller = new();

    public int PollMonitors(List<ISocketMonitor> monitors, int[] activeIndices,
        int activeCount, long deadlineTicks, long nowTicks)
    {
        return _monitorPoller.Poll(monitors, activeIndices, activeCount,
            deadlineTicks, nowTicks);
    }

    public int PollSockets(IReadOnlyList<ISocket> sockets,
        IReadOnlyList<PollEventFlags> eventMasks, int timeoutMs)
    {
        return _socketPoller.Poll(sockets, eventMasks, timeoutMs);
    }

    public int PollSockets(IReadOnlyList<ISocket> sockets, PollEventFlags events,
        int timeoutMs)
    {
        return _socketPoller.Poll(sockets, events, timeoutMs);
    }

    public int ReadySocketCount => _socketPoller.ReadyCount;

    public int ReadySocketIndexAt(int offset)
    {
        return _socketPoller.ReadyIndexAt(offset);
    }

    public PollEventFlags ReadySocketMaskAt(int offset)
    {
        return _socketPoller.ReadyMaskAt(offset);
    }

    public void Dispose()
    {
        _socketPoller.Dispose();
        _monitorPoller.Dispose();
    }
}
