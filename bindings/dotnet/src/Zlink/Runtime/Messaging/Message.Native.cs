// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

// Continuation of Message; the type summary lives on the primary partial in
// Message.cs.
public sealed partial class Message : IDisposable, IAsyncDisposable
{
    internal ref ZlinkMsg Handle
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => ref NativeMsg;
    }

    internal bool IsValid { get; private set; }

    internal void AdoptInitializedNative()
    {
        IsValid = true;
    }

    internal void ReplaceNativeOwned(ref ZlinkMsg source)
    {
        Close();
        NativeMsg = source;
        _knownSize = -1;
        IsValid = true;
        source = default;
    }

    internal void Init()
    {
        if (IsValid)
            return;
        var rc = NativeMethods.zlink_msg_init(ref NativeMsg);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        IsValid = true;
        _knownSize = 0;
    }

    private nuint GetPayloadSize()
    {
        if (_knownSize >= 0)
            return (nuint)_knownSize;
        return IsValid ? NativeMethods.zlink_msg_size(ref NativeMsg) : 0;
    }

    private int GetRefCountCore()
    {
        if (!IsValid)
            return 0;
        var refcount = NativeMethods.zlink_msg_refcnt(ref NativeMsg, out var error);
        ZlinkException.ThrowConfigIfError(error);
        return refcount;
    }

    private unsafe void CopyPayloadToStorage(ReadOnlySpan<byte> data)
    {
        if (data.Length == 0)
            return;
        var dest = NativeMethods.zlink_msg_data(ref NativeMsg);
        if (dest == IntPtr.Zero)
            throw new InvalidOperationException("Message data is null.");
        data.CopyTo(new Span<byte>((void*)dest, data.Length));
    }

    private unsafe Span<byte> AsSpanCore()
    {
        var size = Size;
        if (size == 0)
            return Span<byte>.Empty;
        var data = NativeMethods.zlink_msg_data(ref NativeMsg);
        if (data == IntPtr.Zero)
            return Span<byte>.Empty;
        return new Span<byte>((void*)data, size);
    }

    private unsafe ReadOnlySpan<byte> AsReadOnlySpanCore()
    {
        var size = Size;
        if (size == 0)
            return ReadOnlySpan<byte>.Empty;
        var data = NativeMethods.zlink_msg_data(ref NativeMsg);
        if (data == IntPtr.Zero)
            return ReadOnlySpan<byte>.Empty;
        return new ReadOnlySpan<byte>((void*)data, size);
    }

    private ReadOnlyMemory<byte> AsReadOnlyMemoryCore()
    {
        return ToArray();
    }

    private unsafe bool TryCopyToCore(Span<byte> destination,
        out int bytesWritten)
    {
        var size = GetPayloadSize();
        if (size == 0)
        {
            bytesWritten = 0;
            return true;
        }

        if (size > (nuint)destination.Length)
        {
            bytesWritten = 0;
            return false;
        }

        var data = NativeMethods.zlink_msg_data(ref NativeMsg);
        if (data == IntPtr.Zero)
        {
            bytesWritten = 0;
            return true;
        }

        new ReadOnlySpan<byte>((void*)data, (int)size).CopyTo(destination);
        bytesWritten = (int)size;
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void DisposeCore()
    {
        Close();
        ReturnToPoolOrRelease();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    // HOT PATH: construction boundaries validate size and supply an invalid
    // wrapper; keep the native allocation path free of duplicate checks.
    internal void InitSizeOnInvalidMessage(int size)
    {
        var rc = NativeMethods.zlink_msg_init_size(ref NativeMsg, (nuint)size);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        IsValid = true;
        _knownSize = size;
    }

    /// <summary>
    ///     Creates a new message sharing this message's native payload. Returns a
    ///     new empty message if this message was consumed or disposed. The caller
    ///     owns and must dispose the result.
    /// </summary>
    public Message Copy()
    {
        return IsValid ? CopyCore() : new Message();
    }

    internal Message CopyForSubmit()
    {
        EnsureSubmittable();
        return CopyCore();
    }

    private Message CopyCore()
    {
        var copy = new Message(false);
        CopyNativeTo(ref copy.NativeMsg);
        copy._knownSize = _knownSize;
        copy.IsValid = true;
        return copy;
    }

    /// <summary>
    ///     Creates a new message holding an independent copy of this payload.
    /// </summary>
    public Message Clone()
    {
        return new Message(AsReadOnlySpanCore());
    }

    /// <summary>
    ///     Moves this message's native payload into <paramref name="dest" />,
    ///     replacing its previous payload and leaving this message empty.
    /// </summary>
    public void Move(Message dest)
    {
        ArgumentNullException.ThrowIfNull(dest);
        EnsureValid();
        dest.EnsureValid();

        var knownSize = _knownSize;
        var rc = NativeMethods.zlink_msg_move(ref dest.NativeMsg, ref NativeMsg);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);

        dest._knownSize = knownSize;
        dest.IsValid = true;
        _knownSize = 0;
        IsValid = true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void MoveTo(ref ZlinkMsg dest)
    {
        EnsureSubmittable();
        var rc = NativeMethods.zlink_msg_init(ref dest);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        try
        {
            rc = NativeMethods.zlink_msg_move(ref dest, ref NativeMsg);
            if (rc != 0)
                throw ZlinkException.CreateConfigException((ConfigResult)rc);

            Invalidate();
        }
        catch
        {
            NativeMethods.zlink_msg_close(ref dest);
            throw;
        }
    }

    internal void RestoreFrom(ref ZlinkMsg src)
    {
        if (IsValid)
            throw new InvalidOperationException(
                "RestoreFrom requires an invalid message state.");

        _knownSize = -1;
        var rc = NativeMethods.zlink_msg_init(ref NativeMsg);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        try
        {
            rc = NativeMethods.zlink_msg_move(ref NativeMsg, ref src);
            if (rc != 0)
                throw ZlinkException.CreateConfigException((ConfigResult)rc);
            IsValid = true;
            _knownSize = -1;
        }
        catch
        {
            NativeMethods.zlink_msg_close(ref NativeMsg);
            throw;
        }
    }

    internal void CopyToForSubmit(ref ZlinkMsg dest)
    {
        EnsureSubmittable();
        CopyNativeTo(ref dest);
    }

    private void CopyNativeTo(ref ZlinkMsg dest)
    {
        var rc = NativeMethods.zlink_msg_init(ref dest);
        if (rc != 0)
            throw ZlinkException.CreateConfigException((ConfigResult)rc);
        try
        {
            rc = NativeMethods.zlink_msg_copy(ref dest, ref NativeMsg);
            if (rc != 0)
                throw ZlinkException.CreateConfigException((ConfigResult)rc);
        }
        catch
        {
            NativeMethods.zlink_msg_close(ref dest);
            throw;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void DetachAfterSend()
    {
        EnsureValid();
        Invalidate();
    }

    internal void ConsumeAfterSuccessfulSubmit()
    {
        EnsureValid();
        NativeMethods.zlink_msg_close(ref NativeMsg);
        Invalidate();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void DisposeNativeOwned()
    {
        DisposeCore();
    }

    private void Close()
    {
        if (!IsValid)
            return;
        NativeMethods.zlink_msg_close(ref NativeMsg);
        Invalidate();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void Invalidate()
    {
        NativeMsg = default;
        IsValid = false;
        _knownSize = -1;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureValid()
    {
        if (!IsValid)
            throw new ObjectDisposedException(nameof(Message));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureSubmittable()
    {
        if (!IsValid)
            throw new ZlinkSubmitException(SubmitResult.InvalidHandle, ErrorCode.EFault);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int GetNativeSize(ref ZlinkMsg msg)
    {
        return checked((int)NativeMethods.zlink_msg_size(ref msg));
    }

    private void InitializeManagedCopy(ReadOnlySpan<byte> data)
    {
        // Hot path: Message.From(...) feeds request/reply submit loops. Store
        // the caller snapshot directly in native message storage so submit can
        // move it without a second managed-to-native payload copy.
        InitSizeOnInvalidMessage(data.Length);
        if (data.Length != 0)
            CopyPayloadToStorage(data);
        _knownSize = data.Length;
    }
}
