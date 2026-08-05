// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Static helpers that wait for events across several sockets or monitors at
///     once.
/// </summary>
public static partial class ZlinkPoll
{
    /// <summary>
    ///     Waits up to <paramref name="timeoutMs" /> milliseconds for any of
    ///     <paramref name="sockets" /> to become readable; a negative timeout blocks
    ///     indefinitely.
    /// </summary>
    /// <returns>The number of ready sockets; 0 on timeout.</returns>
    public static int Poll(IReadOnlyList<IZlinkSocket> sockets, int timeoutMs)
    {
        return PollCore(sockets, timeoutMs);
    }

    /// <summary>
    ///     Waits up to <paramref name="timeoutMs" /> milliseconds for the
    ///     <paramref name="events" /> requested per socket, writing the events that
    ///     fired into <paramref name="revents" /> at matching indexes.
    /// </summary>
    /// <returns>The number of sockets with events; 0 on timeout.</returns>
    public static int Poll(IReadOnlyList<IZlinkSocket> sockets,
        IReadOnlyList<PollEventFlags> events, Span<PollEventFlags> revents,
        int timeoutMs)
    {
        return PollCore(sockets, events, revents, timeoutMs);
    }

    /// <summary>
    ///     Waits up to <paramref name="timeoutMs" /> milliseconds for any of
    ///     <paramref name="monitors" /> to have a pending event; a negative timeout
    ///     blocks indefinitely.
    /// </summary>
    /// <returns>The number of ready monitors; 0 on timeout.</returns>
    public static int Poll(IReadOnlyList<ISocketMonitor> monitors,
        int timeoutMs)
    {
        return PollCore(monitors, timeoutMs);
    }

    /// <summary>
    ///     Waits up to <paramref name="timeoutMs" /> milliseconds for the
    ///     <paramref name="events" /> requested per monitor, writing the events that
    ///     fired into <paramref name="revents" /> at matching indexes.
    /// </summary>
    /// <returns>The number of monitors with events; 0 on timeout.</returns>
    public static int Poll(IReadOnlyList<ISocketMonitor> monitors,
        IReadOnlyList<PollEventFlags> events, Span<PollEventFlags> revents,
        int timeoutMs)
    {
        return PollCore(monitors, events, revents, timeoutMs);
    }
}