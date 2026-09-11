// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class RequestReplySupport
{
    internal static Message[] CloneParts(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        var cloned = new Message[parts.Count];
        var built = 0;
        try
        {
            for (; built < parts.Count; built++)
                cloned[built] = parts[built].Copy();
            return cloned;
        }
        catch
        {
            for (var i = 0; i < built; i++)
                cloned[i].Dispose();
            throw;
        }
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

    internal static void ConsumeParts(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 1)
        {
            parts[0].ConsumeAfterSuccessfulSubmit();
            return;
        }
        if (parts.Count == 2)
        {
            parts[0].ConsumeAfterSuccessfulSubmit();
            if (!ReferenceEquals(parts[0], parts[1]))
                parts[1].ConsumeAfterSuccessfulSubmit();
            return;
        }

        var consumed = new HashSet<Message>(
            ReferenceEqualityComparer.Instance);
        foreach (var part in parts)
            if (consumed.Add(part))
                part.ConsumeAfterSuccessfulSubmit();
    }

    internal static void SubmitPreservingOnFailure<T>(
        IReadOnlyList<Message> parts, ref T submitter)
        where T : struct, INativeMessageSubmitter<T>
    {
        EnsureParts(parts, nameof(parts));
        ZlinkMsg[]? rented = null;
        var nativeParts = parts.Count <= NativeMessageParts.StackPartLimit
            ? stackalloc ZlinkMsg[NativeMessageParts.StackPartLimit]
            : rented = ArrayPool<ZlinkMsg>.Shared.Rent(parts.Count);
        var built = 0;
        try
        {
            // Core consumes the whole native record on every returned result.
            // Preserve the managed originals until admission succeeds.
            for (; built < parts.Count; built++)
                parts[built].CopyTo(ref nativeParts[built]);
            var rc = T.Submit(ref submitter, nativeParts[..built]);
            built = 0;
            if (rc != 0)
                throw new ZlinkSubmitException((SubmitResult)rc,
                    NativeMethods.zlink_errno());
            ConsumeParts(parts);
        }
        finally
        {
            for (var i = 0; i < built; i++)
                NativeMethods.zlink_msg_close(ref nativeParts[i]);
            if (rented != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rented);
        }
    }
}

internal interface INativeMessageSubmitter<T> where T : struct, INativeMessageSubmitter<T>
{
    static abstract int Submit(ref T self, Span<ZlinkMsg> parts);
}
