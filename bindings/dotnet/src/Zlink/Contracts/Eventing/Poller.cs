// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Multiplexes sockets, file descriptors, and timers, reporting which become
///     ready on a single wait.
/// </summary>
public interface IPoller : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Gets the number of registered sources.
    /// </summary>
    int Size { get; }

    /// <summary>
    ///     Registers <paramref name="socket" /> to be watched for
    ///     <paramref name="events" />; <paramref name="slot" /> is a caller token
    ///     echoed back in the matching <see cref="PollEvent" />.
    /// </summary>
    void Add(IZlinkSocket socket, PollEventFlags events, nuint slot);

    /// <summary>
    ///     Registers raw file descriptor <paramref name="fd" /> to be watched for
    ///     <paramref name="events" />; <paramref name="slot" /> is echoed back in the
    ///     matching <see cref="PollEvent" />.
    /// </summary>
    void AddFd(int fd, PollEventFlags events, nuint slot);

    /// <summary>
    ///     Registers <paramref name="timer" />; its expirations surface as poll
    ///     events tagged with <paramref name="slot" />.
    /// </summary>
    void Add(IZlinkTimer timer, nuint slot);

    /// <summary>
    ///     Changes the watched events for an already-registered socket.
    /// </summary>
    void Modify(IZlinkSocket socket, PollEventFlags events);

    /// <summary>
    ///     Changes the watched events for an already-registered file descriptor.
    /// </summary>
    void ModifyFd(int fd, PollEventFlags events);

    /// <summary>
    ///     Unregisters <paramref name="socket" />.
    /// </summary>
    /// <returns>true when it was registered; otherwise false.</returns>
    bool Remove(IZlinkSocket socket);

    /// <summary>
    ///     Unregisters <paramref name="timer" />.
    /// </summary>
    /// <returns>true when it was registered; otherwise false.</returns>
    bool Remove(IZlinkTimer timer);

    /// <summary>
    ///     Unregisters file descriptor <paramref name="fd" />.
    /// </summary>
    /// <returns>true when it was registered; otherwise false.</returns>
    bool Remove(int fd);

    /// <summary>
    ///     Unregisters every source.
    /// </summary>
    void Clear();

    /// <summary>
    ///     Closes the poller and releases its resources.
    /// </summary>
    void Close();

    /// <summary>
    ///     Waits up to <paramref name="timeout" /> for sources to become ready,
    ///     writing up to <paramref name="destination" />.Length results into it.
    /// </summary>
    /// <returns>The number of ready sources written; 0 on timeout.</returns>
    int Wait(Span<PollEvent> destination, TimeSpan timeout);
}