// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A handle to a running background thread (see
///     <see cref="Zlink.CreateThread" />, which starts it).
/// </summary>
public interface IZlinkThread : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Blocks the caller until the thread finishes running its task; repeated
    ///     calls are no-ops.
    /// </summary>
    void Join();

    /// <summary>
    ///     Releases the thread handle, joining first if it is still running.
    /// </summary>
    void Close();
}