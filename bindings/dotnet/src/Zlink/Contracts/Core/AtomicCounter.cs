// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     A thread-safe integer counter that can be shared across threads.
/// </summary>
public interface IAtomicCounter : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Gets the current value with an atomic read.
    /// </summary>
    int Value { get; }

    /// <summary>
    ///     Atomically sets the value.
    /// </summary>
    void Set(int value);

    /// <summary>
    ///     Increments the counter and returns the new value.
    /// </summary>
    int Increment();

    /// <summary>
    ///     Decrements the counter and returns the new value.
    /// </summary>
    int Decrement();
}