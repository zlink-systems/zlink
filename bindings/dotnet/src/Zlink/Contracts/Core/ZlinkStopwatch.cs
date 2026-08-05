// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A high-resolution elapsed-time stopwatch.
/// </summary>
public interface IZlinkStopwatch : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Returns the elapsed time so far, in microseconds, without stopping.
    /// </summary>
    ulong Intermediate();

    /// <summary>
    ///     Stops the stopwatch and returns the total elapsed time in microseconds.
    /// </summary>
    ulong Stop();
}