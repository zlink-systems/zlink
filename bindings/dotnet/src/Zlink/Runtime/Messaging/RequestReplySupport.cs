// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class RequestReplySupport
{
    internal static Message[] CloneParts(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        var cloned = new Message[parts.Count];
        for (var i = 0; i < parts.Count; i++)
            cloned[i] = parts[i].Copy();
        return cloned;
    }

    internal static void EnsureParts(IReadOnlyList<Message> parts,
        string paramName)
    {
        if (parts == null)
            throw new ArgumentNullException(paramName);
        if (parts.Count == 0)
            throw new ArgumentException("Parts must not be empty.", paramName);
    }

    internal static uint NormalizeTimeout(TimeSpan timeout)
    {
        return BoundaryValidation.EncodeTimeoutMilliseconds(timeout,
            nameof(timeout));
    }

    internal static uint NormalizeRequestTimeout(TimeSpan timeout,
        TimeSpan defaultTimeout)
    {
        var effective = timeout == TimeSpan.Zero
            ? defaultTimeout
            : timeout;
        return BoundaryValidation.EncodeTimeoutMilliseconds(effective,
            nameof(timeout));
    }

    internal static void DisposeParts(IEnumerable<Message> parts)
    {
        foreach (var part in parts)
            part.Dispose();
    }

    internal static void SubmitClonedParts(IReadOnlyList<Message> parts,
        NativePartSubmitter submit)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        for (var i = 0; i < parts.Count; i++)
        {
            ZlinkMsg nativePart = default;
            parts[i].MoveTo(ref nativePart);
            var submitReturned = false;
            try
            {
                var rc = submit(ref nativePart, i + 1 < parts.Count
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final);
                submitReturned = true;
                if (rc != 0)
                    throw ZlinkException.CreateSubmitException(
                        NativeMethods.zlink_errno());
            }
            finally
            {
                // A returned Core submit result consumes this native part on
                // both success and failure. Restore only when the managed
                // delegate itself failed before returning a Core result.
                if (!submitReturned)
                    parts[i].RestoreFrom(ref nativePart);
            }
        }
    }

    internal static void SubmitOwnedSinglePart(Message part,
        NativePartSubmitter submit)
    {
        if (part == null)
            throw new ArgumentNullException(nameof(part));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        var submitter = new DelegateSinglePartSubmitter(submit);
        var rc = SinglePartSubmit.Submit(part, ref submitter);
        if (rc != 0)
            throw ZlinkException.CreateSubmitException(NativeMethods.zlink_errno());
    }

    private readonly struct DelegateSinglePartSubmitter
        : INativeSinglePartSubmitter<DelegateSinglePartSubmitter>
    {
        private readonly NativePartSubmitter _submit;

        internal DelegateSinglePartSubmitter(NativePartSubmitter submit)
        {
            _submit = submit;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref DelegateSinglePartSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return submitter._submit(ref nativePart,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }

    internal static void SubmitOwnedParts(IReadOnlyList<Message> parts,
        NativePartSubmitter submit)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        if (parts.Count == 1)
        {
            SubmitOwnedSinglePart(parts[0], submit);
            return;
        }

        Message[]? copiedParts = null;
        var sourceParts = NativeMessageParts.AsSpan(parts, ref copiedParts);
        ZlinkMsg[]? rentedNative = null;
        // Hot path: request/reply messages are normally one or two parts. Keep
        // native descriptors on the stack for that case and rent only for larger
        // multipart frames.
        var nativeParts = sourceParts.Length <= NativeMessageParts.StackPartLimit
            ? stackalloc ZlinkMsg[NativeMessageParts.StackPartLimit]
            : rentedNative = ArrayPool<ZlinkMsg>.Shared.Rent(sourceParts.Length);
        nativeParts = nativeParts[..sourceParts.Length];

        var built = 0;
        var consumed = 0;
        try
        {
            NativeMessageParts.MoveToNative(sourceParts, nativeParts,
                nameof(parts), ref built);
            for (var i = 0; i < built; i++)
            {
                var rc = submit(ref nativeParts[i], i + 1 < built
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final);
                consumed = i + 1;
                if (rc == 0)
                    continue;

                throw ZlinkException.CreateSubmitException(
                    NativeMethods.zlink_errno());
            }
        }
        catch
        {
            NativeMessageParts.RestoreManaged(sourceParts, nativeParts,
                consumed, built - consumed);
            throw;
        }
        finally
        {
            if (rentedNative != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rentedNative);
        }
    }

    internal delegate int NativePartSubmitter(
        ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag);
}
