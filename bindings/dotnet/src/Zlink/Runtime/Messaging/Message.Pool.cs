// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

// Continuation of Message; the type summary lives on the primary partial in
// Message.cs.
public sealed partial class Message : IDisposable, IAsyncDisposable
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static Message RentForNativeReceive()
    {
        // HOT PATH: TopicMessage owns a received native frame until disposal.
        // A returned wrapper has no live native handle, so the thread-local
        // pool can reuse its managed identity without weakening that ownership.
        return RentFromPool();
    }

    // Pool-aware adoption used by the routed single-part receive fast path,
    // where Message lifetime is bounded by the immediate caller scope.
    internal static Message AdoptNativeFromPool(ref ZlinkMsg source)
    {
        var result = RentFromPool();
        result._msg = source;
        result.IsValid = true;
        result._knownSize = -1;
        source = default;
        return result;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static Message RentFromPool()
    {
        var pool = t_pool;
        var count = t_poolCount;
        if (pool != null && count > 0)
        {
            count--;
            var result = pool[count];
            pool[count] = null!;
            t_poolCount = count;
            result.Invalidate(clearHandle: true);
            result._pooled = true;
            return result;
        }

        return new Message(false) { _pooled = true };
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void TryReturnToPool()
    {
        if (!_pooled)
            return;
        // Defensive: only return when fully released with no native handle
        // pending close.
        if (IsValid)
            return;
        _pooled = false;
        var pool = t_pool;
        if (pool == null)
        {
            pool = new Message[PoolCapacity];
            t_pool = pool;
        }

        var count = t_poolCount;
        if (count >= PoolCapacity)
            return;
        pool[count] = this;
        t_poolCount = count + 1;
    }
}
