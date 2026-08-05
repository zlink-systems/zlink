// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Context-wide options that govern the I/O threads and defaults shared by
///     every socket created from the context.
/// </summary>
public interface IContextOptions
{
    /// <summary>
    ///     Gets or sets the number of background I/O threads serving the context.
    /// </summary>
    int IoThreads { get; set; }

    /// <summary>
    ///     Gets or sets the maximum number of sockets the context may create.
    /// </summary>
    int MaxSockets { get; set; }

    /// <summary>
    ///     Gets the largest value <see cref="MaxSockets" /> may take on this build.
    /// </summary>
    int SocketLimit { get; }

    /// <summary>
    ///     Gets or sets the OS scheduling priority of the context's I/O threads.
    /// </summary>
    int ThreadPriority { get; set; }

    /// <summary>
    ///     Gets or sets the OS scheduling policy of the context's I/O threads.
    /// </summary>
    int ThreadSchedulingPolicy { get; set; }

    /// <summary>
    ///     Gets or sets the default maximum inbound message size, in bytes, for new
    ///     sockets.
    /// </summary>
    int MaxMessageSize { get; set; }

    /// <summary>
    ///     Gets the size of the context's message worker thread pool.
    /// </summary>
    int MessageThreadSize { get; }

    /// <summary>
    ///     Gets or sets whether the context blocks on termination until queued
    ///     messages have been sent.
    /// </summary>
    bool Blocky { get; set; }

    /// <summary>
    ///     Gets or sets the profile used to size high-water marks automatically;
    ///     see <see cref="Systems.Zlink.AutoHwmProfile" />.
    /// </summary>
    AutoHwmProfile AutoHwmProfile { get; set; }

    /// <summary>
    ///     Gets or sets the assumed message size, in bytes, used when auto-sizing
    ///     high-water marks.
    /// </summary>
    ulong AutoHwmMessageUnitBytes { get; set; }

    /// <summary>
    ///     Gets or sets whether high-water marks are sized automatically.
    /// </summary>
    bool AutoHwmEnabled { get; set; }

    /// <summary>
    ///     Gets or sets the minimum delay between automatic high-water-mark
    ///     recalculations.
    /// </summary>
    TimeSpan AutoHwmRecalcDebounce { get; set; }

    /// <summary>
    ///     Gets or sets the prefix applied to the names of threads the context
    ///     creates.
    /// </summary>
    string ThreadNamePrefix { get; set; }

    /// <summary>
    ///     Pins the context's I/O threads to also run on CPU core
    ///     <paramref name="cpu" />.
    /// </summary>
    void AddThreadAffinityCpu(int cpu);

    /// <summary>
    ///     Removes CPU core <paramref name="cpu" /> from the context's I/O thread
    ///     affinity.
    /// </summary>
    void RemoveThreadAffinityCpu(int cpu);
}
