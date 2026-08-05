// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.CompilerServices;
using System.Text;

namespace Systems.Zlink;

/// <summary>
///     Owns one zlink message payload.
/// </summary>
/// <remarks>
///     A message is disposable because it may own native storage. Span-returning
///     APIs expose storage owned by this instance and are valid only while the
///     message remains undisposed.
/// </remarks>
public sealed partial class Message : IDisposable, IAsyncDisposable
{
    /// <summary>
    ///     Create an empty message.
    /// </summary>
    public Message()
    {
        Init();
    }

    /// <summary>
    ///     Create a message with writable payload storage of <paramref name="size" /> bytes.
    /// </summary>
    /// <param name="size">Payload size in bytes. The value must be non-negative.</param>
    /// <exception cref="ArgumentOutOfRangeException">
    ///     <paramref name="size" /> is negative.
    /// </exception>
    public Message(int size)
    {
        if (size < 0)
            throw new ArgumentOutOfRangeException(nameof(size));
        InitSizeOnInvalidMessage(size);
    }

    /// <summary>
    ///     Create a message containing a snapshot copy of <paramref name="data" />.
    /// </summary>
    public Message(ReadOnlySpan<byte> data) : this(data.Length)
    {
        if (data.Length == 0)
            return;
        CopyPayloadToStorage(data);
    }

    /// <summary>
    ///     Create a message containing a snapshot copy of <paramref name="data" />.
    /// </summary>
    public Message(ReadOnlyMemory<byte> data) : this(data.Span)
    {
    }

    /// <summary>
    ///     Gets the payload size in bytes.
    /// </summary>
    public int Size => GetSizeCore();

    /// <summary>
    ///     Gets whether the payload is empty.
    /// </summary>
    public bool IsEmpty => Size == 0;

    /// <summary>
    ///     Gets the native payload reference count.
    /// </summary>
    /// <remarks>
    ///     Newly created messages own their native storage directly. A moved or
    ///     disposed message cannot report a reference count.
    /// </remarks>
    public int RefCount => GetRefCountCore();

    /// <summary>
    ///     Releases the payload storage owned by this message. Disposal is
    ///     synchronous; this returns an already-completed task for callers that
    ///     await disposal.
    /// </summary>
    public ValueTask DisposeAsync()
    {
        Dispose();
        return ValueTask.CompletedTask;
    }

    /// <summary>
    ///     Releases the payload storage owned by this message.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Dispose()
    {
        DisposeCore();
    }

    /// <summary>
    ///     Allocate a message with writable payload storage.
    /// </summary>
    /// <param name="size">Payload size in bytes. The value must be non-negative.</param>
    /// <exception cref="ArgumentOutOfRangeException">
    ///     <paramref name="size" /> is negative.
    /// </exception>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Message Allocate(int size)
    {
        if (size < 0)
            throw new ArgumentOutOfRangeException(nameof(size));
        return AllocateCoreValidated(size);
    }

    /// <summary>
    ///     Returns a writable view of the message payload.
    /// </summary>
    /// <remarks>
    ///     The returned span is backed by storage owned by this message. It must
    ///     not be used after the message is disposed or moved.
    /// </remarks>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Span<byte> AsSpan()
    {
        return AsSpanCore();
    }

    /// <summary>
    ///     Returns a new byte array containing a copy of the payload.
    /// </summary>
    public byte[] ToArray()
    {
        return AsReadOnlySpan().ToArray();
    }

    /// <summary>
    ///     Returns a read-only view of the message payload.
    /// </summary>
    /// <remarks>
    ///     The returned span is backed by storage owned by this message. It must
    ///     not be used after the message is disposed or moved.
    /// </remarks>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ReadOnlySpan<byte> AsReadOnlySpan()
    {
        return AsReadOnlySpanCore();
    }

    /// <summary>
    ///     Returns a read-only memory view of the payload.
    /// </summary>
    /// <remarks>
    ///     Native-backed messages are copied into managed memory because
    ///     <see cref="ReadOnlyMemory{T}" /> cannot safely reference native storage.
    /// </remarks>
    public ReadOnlyMemory<byte> AsReadOnlyMemory()
    {
        return AsReadOnlyMemoryCore();
    }

    /// <summary>
    ///     Copies the payload into <paramref name="destination" />.
    /// </summary>
    /// <returns>The number of bytes written.</returns>
    public int CopyTo(Span<byte> destination)
    {
        if (!TryCopyTo(destination, out var bytesWritten))
            throw new ArgumentException("Destination buffer is too small.",
                nameof(destination));
        return bytesWritten;
    }

    /// <summary>
    ///     Copies the payload to an <see cref="IBufferWriter{T}" />.
    /// </summary>
    /// <returns>The number of bytes written.</returns>
    public int CopyTo(IBufferWriter<byte> destination)
    {
        if (destination == null)
            throw new ArgumentNullException(nameof(destination));

        var size = Size;
        var span = destination.GetSpan(size).Slice(0, size);
        var written = CopyTo(span);
        destination.Advance(written);
        return written;
    }

    /// <summary>
    ///     Attempts to copy the payload into <paramref name="destination" />.
    /// </summary>
    /// <returns>true when the destination was large enough; otherwise false.</returns>
    public bool TryCopyTo(Span<byte> destination, out int bytesWritten)
    {
        return TryCopyToCore(destination, out bytesWritten);
    }

    /// <summary>
    ///     Create a message containing a snapshot copy of <paramref name="data" />.
    ///     The caller may freely mutate or discard <paramref name="data" /> after
    ///     this call returns; the message holds its own copy of the payload.
    /// </summary>
    public static Message From(byte[] data)
    {
        if (data == null)
            throw new ArgumentNullException(nameof(data));
        var message = new Message(false);
        message.InitializeManagedCopy(data.AsSpan());
        return message;
    }

    /// <summary>
    ///     Create a message holding an independent snapshot copy of
    ///     <paramref name="data" />; see <see cref="From(byte[])" /> for copy
    ///     semantics.
    /// </summary>
    public static Message From(ReadOnlySpan<byte> data)
    {
        var message = new Message(false);
        message.InitializeManagedCopy(data);
        return message;
    }

    /// <summary>
    ///     Create a message holding an independent snapshot copy of
    ///     <paramref name="data" />; see <see cref="From(byte[])" /> for copy
    ///     semantics.
    /// </summary>
    public static Message From(ReadOnlyMemory<byte> data)
    {
        var message = new Message(false);
        message.InitializeManagedCopy(data.Span);
        return message;
    }

    /// <summary>
    ///     Create a message holding an independent snapshot copy of
    ///     <paramref name="data" />; see <see cref="From(byte[])" /> for copy
    ///     semantics.
    /// </summary>
    public static Message From(ReadOnlySequence<byte> data)
    {
        if (data.IsSingleSegment)
            return From(data.First);
        return From(data.ToArray());
    }

    /// <summary>
    ///     Create a message containing a copy of another message payload.
    /// </summary>
    public static Message From(Message source)
    {
        if (source == null)
            throw new ArgumentNullException(nameof(source));
        return source.Copy();
    }

    /// <summary>
    ///     Encode <paramref name="value" /> as UTF-8 and copy it into a message.
    /// </summary>
    public static Message From(string value)
    {
        return From(value, Encoding.UTF8);
    }

    /// <summary>
    ///     Encode <paramref name="value" /> with <paramref name="encoding" /> and
    ///     copy it into a message.
    /// </summary>
    public static Message From(string value, Encoding encoding)
    {
        if (value == null)
            throw new ArgumentNullException(nameof(value));
        if (encoding == null)
            throw new ArgumentNullException(nameof(encoding));
        return From(encoding.GetBytes(value));
    }

    /// <summary>
    ///     Decode the payload as UTF-8.
    /// </summary>
    public string GetString()
    {
        return GetString(Encoding.UTF8);
    }

    /// <summary>
    ///     Decode the payload with <paramref name="encoding" />.
    /// </summary>
    public string GetString(Encoding encoding)
    {
        if (encoding == null)
            throw new ArgumentNullException(nameof(encoding));
        return encoding.GetString(AsReadOnlySpan());
    }
}
