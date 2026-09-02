// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A timer that fires on an interval and can be awaited or polled.
/// </summary>
public interface IZlinkTimer : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Starts the timer firing once per <paramref name="interval" />;
    ///     <paramref name="repeatCount" /> sets how many times it fires.
    /// </summary>
    void Start(TimeSpan interval, ulong repeatCount);

    /// <summary>
    ///     Stops the timer; it can be restarted with <see cref="Start" />.
    /// </summary>
    void Stop();

    /// <summary>
    ///     Receives the next expiration as the cumulative fire count, or null when
    ///     none is pending and <see cref="RecvFlags.DontWait" /> is set.
    /// </summary>
    ulong? Recv(RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Closes the timer and releases its resources.
    /// </summary>
    void Close();
}
