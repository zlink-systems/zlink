// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal interface INativeSinglePartSubmitter<TSubmit>
    where TSubmit : struct, INativeSinglePartSubmitter<TSubmit>
{
    static abstract int Submit(ref TSubmit submitter, ref ZlinkMsg nativePart);
}

internal static class SinglePartSubmit
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static int Submit<TSubmit>(Message message, ref TSubmit submitter)
        where TSubmit : struct, INativeSinglePartSubmitter<TSubmit>
    {
        // HOT PATH: zlink_msg_init inside MoveTo initializes all 64 opaque
        // bytes before any read. Avoid clearing the same stack storage first.
        Unsafe.SkipInit(out ZlinkMsg nativePart);
        var shouldRestore = false;
        try
        {
            message.MoveTo(ref nativePart);
            shouldRestore = true;
            var rc = TSubmit.Submit(ref submitter, ref nativePart);
            // Core consumes the native part on every returned submit result,
            // including DONTWAIT backpressure on STREAM.
            shouldRestore = false;
            return rc;
        }
        catch
        {
            if (shouldRestore)
                message.RestoreFrom(ref nativePart);
            throw;
        }
    }
}
