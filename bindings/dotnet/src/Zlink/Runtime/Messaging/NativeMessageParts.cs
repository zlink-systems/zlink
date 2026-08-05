// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class NativeMessageParts
{
    internal const int StackPartLimit = 8;

    internal static ReadOnlySpan<Message> AsSpan(IReadOnlyList<Message> parts,
        ref Message[]? copiedParts)
    {
        if (parts is Message[] array)
            return array;

        if (parts is List<Message> list)
            return CollectionsMarshal.AsSpan(list);

        copiedParts = new Message[parts.Count];
        for (var i = 0; i < copiedParts.Length; i++)
            copiedParts[i] = parts[i];
        return copiedParts;
    }

    internal static void MoveToNative(ReadOnlySpan<Message> parts,
        Span<ZlinkMsg> nativeParts, string paramName, ref int built)
    {
        // Validate before the first move so a bad part cannot leave earlier
        // messages invalidated with no native owner to restore from.
        for (var i = 0; i < parts.Length; i++)
            if (parts[i] == null)
                throw new ArgumentException(
                    "Parts must not contain null messages.", paramName);

        for (var i = 0; i < parts.Length; i++)
        {
            parts[i].MoveTo(ref nativeParts[i]);
            built++;
        }
    }

    internal static void RestoreManaged(ReadOnlySpan<Message> parts,
        Span<ZlinkMsg> nativeParts, int start, int count)
    {
        // Restore in reverse construction order to mirror the native array
        // ownership transfer when multipart setup fails partway through.
        for (var i = start + count - 1; i >= start; i--)
            parts[i].RestoreFrom(ref nativeParts[i]);
    }

    internal static unsafe void SubmitClonedVector(IReadOnlyList<Message> parts,
        string paramName, NativePartVectorSubmitter submit,
        Action<int>? throwIfError)
    {
        RequestReplySupport.EnsureParts(parts, paramName);
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        var cloned = RequestReplySupport.CloneParts(parts);
        ZlinkMsg[]? rented = null;
        var nativeParts = cloned.Length <= StackPartLimit
            ? stackalloc ZlinkMsg[StackPartLimit]
            : rented = ArrayPool<ZlinkMsg>.Shared.Rent(cloned.Length);
        nativeParts = nativeParts[..cloned.Length];
        var built = 0;
        try
        {
            MoveToNative(cloned, nativeParts, paramName, ref built);
            fixed (ZlinkMsg* nativePtr = nativeParts)
            {
                var rc = submit((IntPtr)nativePtr, (nuint)built);
                throwIfError?.Invoke(rc);
            }
        }
        finally
        {
            for (var i = 0; i < built; i++)
                NativeMethods.zlink_msg_close(ref nativeParts[i]);
            RequestReplySupport.DisposeParts(cloned);
            if (rented != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rented);
        }
    }

    internal delegate int NativePartVectorSubmitter(IntPtr nativeParts,
        nuint partCount);
}
