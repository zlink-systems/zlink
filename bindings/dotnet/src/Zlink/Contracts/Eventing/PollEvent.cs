// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     One ready source reported by an <see cref="IPoller" /> wait.
/// </summary>
public readonly partial struct PollEvent
{
    /// <summary>
    ///     Gets whether the ready source is a socket, file descriptor, or timer.
    /// </summary>
    public PollSourceKind SourceKind { get; }

    /// <summary>
    ///     Gets the caller token supplied when the source was registered.
    /// </summary>
    public nuint Slot { get; }

    /// <summary>
    ///     Gets the returned poll events.
    /// </summary>
    public PollEventFlags Revents { get; }

    /// <summary>
    ///     Gets the file descriptor.
    /// </summary>
    public int Fd { get; }
}