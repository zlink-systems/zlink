// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

public sealed unsafe partial class Message
{
    private const int PoolCapacity = 256;

    [ThreadStatic] private static Message[]? t_pool;

    [ThreadStatic] private static int t_poolCount;

    private int _knownSize = -1;

    // Core keeps payloads of up to 29 bytes inside zlink_msg_t itself, and
    // payload views point into it. The struct therefore lives in native memory
    // that the GC never moves. The block belongs to this wrapper for its whole
    // managed life, including time spent in the thread-static pool, and is
    // freed only by ReleaseNativeStorage.
    private ZlinkMsg* _msg = AllocateNativeStorage();
    private enum PoolState : byte
    {
        Owned,
        Rented,
        Returned
    }

    private PoolState _poolState;

    internal Message(bool init)
    {
        if (init)
            Init();
    }

    /// <summary>
    ///     Releases the native message storage when <see cref="Dispose" /> was
    ///     not called.
    /// </summary>
    ~Message()
    {
        ReleaseNativeStorage();
    }

    private static ZlinkMsg* AllocateNativeStorage()
    {
        var msg = (ZlinkMsg*)NativeMemory.AlignedAlloc(
            (nuint)sizeof(ZlinkMsg), (nuint)IntPtr.Size);
        *msg = default;
        return msg;
    }

    // The one release rule for the native block, shared by Dispose (when the
    // wrapper is not kept by the pool) and the finalizer (when Dispose was
    // missed): close a still-owned payload, then free the block exactly once.
    private void ReleaseNativeStorage()
    {
        var msg = _msg;
        if (msg == null)
            return;
        if (IsValid)
            NativeMethods.zlink_msg_close(ref *msg);
        _msg = null;
        IsValid = false;
        _knownSize = -1;
        NativeMemory.AlignedFree(msg);
    }

    private ref ZlinkMsg NativeMsg
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get
        {
            var msg = _msg;
            if (msg == null)
                throw new ObjectDisposedException(nameof(Message));
            return ref *msg;
        }
    }
}
