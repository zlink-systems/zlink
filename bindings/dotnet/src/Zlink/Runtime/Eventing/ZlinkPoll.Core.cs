// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public static partial class ZlinkPoll
{
    [ThreadStatic] private static ZlinkPollItemUnix[]? _unixItems;

    [ThreadStatic] private static ZlinkPollItemWindows[]? _windowsItems;

    private static int PollCore(IReadOnlyList<IZlinkSocket> sockets,
        int timeoutMs)
    {
        if (sockets == null)
            throw new ArgumentNullException(nameof(sockets));
        return PollSocketsCore(sockets, null, default, timeoutMs,
            writeRevents: false);
    }

    private static int PollCore(IReadOnlyList<IZlinkSocket> sockets,
        IReadOnlyList<PollEventFlags> events, Span<PollEventFlags> revents,
        int timeoutMs)
    {
        if (sockets == null)
            throw new ArgumentNullException(nameof(sockets));
        return PollSocketsCore(sockets, events, revents, timeoutMs,
            writeRevents: true);
    }

    private static int PollCore(IReadOnlyList<ISocketMonitor> monitors,
        int timeoutMs)
    {
        if (monitors == null)
            throw new ArgumentNullException(nameof(monitors));
        return PollMonitorsCore(monitors, null, default, timeoutMs,
            writeRevents: false);
    }

    private static int PollCore(IReadOnlyList<ISocketMonitor> monitors,
        IReadOnlyList<PollEventFlags> events, Span<PollEventFlags> revents,
        int timeoutMs)
    {
        if (monitors == null)
            throw new ArgumentNullException(nameof(monitors));
        return PollMonitorsCore(monitors, events, revents, timeoutMs,
            writeRevents: true);
    }

    private static SocketMonitor RequireMonitor(ISocketMonitor monitor,
        string paramName)
    {
        if (monitor == null)
            throw new ArgumentNullException(paramName);
        if (monitor is not SocketMonitor concrete)
            throw new ArgumentException(
                "monitor must be a concrete zlink socket monitor instance",
                paramName);
        return concrete;
    }

    private static int PollSocketsCore(IReadOnlyList<IZlinkSocket> sockets,
        IReadOnlyList<PollEventFlags>? events, Span<PollEventFlags> revents,
        int timeoutMs, bool writeRevents)
    {
        var count = sockets.Count;
        ValidatePollBuffers(count, events, revents, writeRevents);
        if (count <= 0)
            return 0;

        long boundedTimeoutMs = Math.Max(0, timeoutMs);
        if (OperatingSystem.IsWindows())
        {
            var items = EnsureWindowsCapacity(count);
            for (var i = 0; i < count; i++)
            {
                items[i].Socket = SocketInterop
                    .RequireSocket(sockets[i], nameof(sockets)).Handle;
                items[i].Fd = 0;
                items[i].Events = (short)(events == null
                    ? PollEventFlags.PollIn
                    : events[i]);
                items[i].Revents = 0;
            }

            var rc = NativeMethods.zlink_poll(items, count, boundedTimeoutMs,
                out var windowsErrorOut);
            if (rc < 0)
                throw ZlinkException.CreateConfigException((ConfigResult)windowsErrorOut);
            if (writeRevents)
                for (var i = 0; i < count; i++)
                    revents[i] = (PollEventFlags)items[i].Revents;
            return rc;
        }

        var unixItems = EnsureUnixCapacity(count);
        for (var i = 0; i < count; i++)
        {
            unixItems[i].Socket = SocketInterop
                .RequireSocket(sockets[i], nameof(sockets)).Handle;
            unixItems[i].Fd = 0;
            unixItems[i].Events = (short)(events == null
                ? PollEventFlags.PollIn
                : events[i]);
            unixItems[i].Revents = 0;
        }

        var unixRc = NativeMethods.zlink_poll(unixItems, count,
            boundedTimeoutMs, out var errorOut);
        if (unixRc < 0)
            throw ZlinkException.CreateConfigException((ConfigResult)errorOut);
        if (writeRevents)
            for (var i = 0; i < count; i++)
                revents[i] = (PollEventFlags)unixItems[i].Revents;
        return unixRc;
    }

    private static int PollMonitorsCore(
        IReadOnlyList<ISocketMonitor> monitors,
        IReadOnlyList<PollEventFlags>? events, Span<PollEventFlags> revents,
        int timeoutMs, bool writeRevents)
    {
        var count = monitors.Count;
        ValidatePollBuffers(count, events, revents, writeRevents);
        if (count <= 0)
            return 0;

        long boundedTimeoutMs = Math.Max(0, timeoutMs);
        if (OperatingSystem.IsWindows())
        {
            var items = EnsureWindowsCapacity(count);
            for (var i = 0; i < count; i++)
            {
                items[i].Socket = RequireMonitor(monitors[i],
                    nameof(monitors)).Handle;
                items[i].Fd = 0;
                items[i].Events = (short)(events == null
                    ? PollEventFlags.PollIn
                    : events[i]);
                items[i].Revents = 0;
            }

            var rc = NativeMethods.zlink_poll(items, count, boundedTimeoutMs,
                out var windowsErrorOut);
            if (rc < 0)
                throw ZlinkException.CreateConfigException((ConfigResult)windowsErrorOut);
            if (writeRevents)
                for (var i = 0; i < count; i++)
                    revents[i] = (PollEventFlags)items[i].Revents;
            return rc;
        }

        var unixItems = EnsureUnixCapacity(count);
        for (var i = 0; i < count; i++)
        {
            unixItems[i].Socket = RequireMonitor(monitors[i],
                nameof(monitors)).Handle;
            unixItems[i].Fd = 0;
            unixItems[i].Events = (short)(events == null
                ? PollEventFlags.PollIn
                : events[i]);
            unixItems[i].Revents = 0;
        }

        var unixRc = NativeMethods.zlink_poll(unixItems, count,
            boundedTimeoutMs, out var errorOut);
        if (unixRc < 0)
            throw ZlinkException.CreateConfigException((ConfigResult)errorOut);
        if (writeRevents)
            for (var i = 0; i < count; i++)
                revents[i] = (PollEventFlags)unixItems[i].Revents;
        return unixRc;
    }

    private static void ValidatePollBuffers(int count,
        IReadOnlyList<PollEventFlags>? events, Span<PollEventFlags> revents,
        bool writeRevents)
    {
        if (events != null && events.Count < count)
            throw new ArgumentException("events length is too small.",
                nameof(events));
        if (writeRevents && revents.Length < count)
            throw new ArgumentException("revents length is too small.",
                nameof(revents));
    }

    private static ZlinkPollItemUnix[] EnsureUnixCapacity(int count)
    {
        if (_unixItems == null || _unixItems.Length < count)
            _unixItems = new ZlinkPollItemUnix[count];
        return _unixItems;
    }

    private static ZlinkPollItemWindows[] EnsureWindowsCapacity(int count)
    {
        if (_windowsItems == null || _windowsItems.Length < count)
            _windowsItems = new ZlinkPollItemWindows[count];
        return _windowsItems;
    }
}
