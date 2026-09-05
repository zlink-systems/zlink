// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Multiplexes sockets, monitors, file descriptors, and timers, reporting which become
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
    /// <remarks>
    ///     An external loop that drives awaitable SEND retries registers both
    ///     <see cref="PollEventFlags.PollOut" /> and
    ///     <see cref="PollEventFlags.PollCompletion" />. POLLOUT reports writable
    ///     credit; POLLCOMPLETION reserves this poller as the socket's sole
    ///     completion-queue drain owner and is reported to the caller only for
    ///     request completions.
    /// </remarks>
    void Add(IZlinkSocket socket, PollEventFlags events, nuint slot);

    /// <summary>
    ///     Registers a monitor with a caller slot. Only PollIn or None is valid;
    ///     other bits throw ZlinkConfigException with InvalidArgument.
    ///     After readiness, drain monitor events with Recv(DontWait).
    /// </summary>
    void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot);

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
    ///     Changes a registered monitor's mask. Only PollIn or None is valid;
    ///     other bits throw ZlinkConfigException with InvalidArgument.
    /// </summary>
    void Modify(ISocketMonitor monitor, PollEventFlags events);

    /// <summary>
    ///     Changes the watched events for an already-registered file descriptor.
    /// </summary>
    void ModifyFd(int fd, PollEventFlags events);

    /// <summary>
    ///     Unregisters <paramref name="socket" />.
    /// </summary>
    /// <returns>true when it was registered; otherwise false.</returns>
    bool Remove(IZlinkSocket socket);

    /// <summary>Unregisters a monitor.</summary>
    /// <returns>true when it was registered; otherwise false.</returns>
    bool Remove(ISocketMonitor monitor);

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
