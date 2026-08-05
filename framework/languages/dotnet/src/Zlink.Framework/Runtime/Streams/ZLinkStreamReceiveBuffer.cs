using System.Buffers;
using System.Buffers.Binary;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamReceiveBuffer : IDisposable
{
    private const int DefaultCapacity = 4096;
    private readonly long _maxMessageSize;
    private byte[] _buffer = ArrayPool<byte>.Shared.Rent(DefaultCapacity);
    private int _offset;
    private int _length;
    private bool _disposed;

    internal ZLinkStreamReceiveBuffer(long maxMessageSize)
    {
        _maxMessageSize = maxMessageSize;
    }

    internal void Append(ReadOnlySpan<byte> bytes)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (bytes.IsEmpty) return;

        if (bytes.Length > int.MaxValue - _length)
            throw new InvalidDataException("STREAM frame exceeds the supported size.");
        var required = _length + bytes.Length;
        EnsureCapacity(required);
        bytes.CopyTo(_buffer.AsSpan(_offset + _length));
        _length = required;
    }

    internal bool TryTakeFrame(out ZLinkStreamInboundFrame? frame)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        frame = null;
        if (_length < ZLinkStreamFrameCodec.PrefixSize) return false;

        var input = _buffer.AsSpan(_offset, _length);
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(input[..2]);
        var payloadSize = BinaryPrimitives.ReadUInt32BigEndian(input.Slice(2, 4));
        if (payloadSize > int.MaxValue)
            throw new InvalidDataException("STREAM payload length exceeds the supported size.");

        var messageSize = checked((long)headerSize + payloadSize);
        if (_maxMessageSize > 0 && messageSize > _maxMessageSize)
            throw new InvalidDataException("STREAM frame exceeds MaxMessageSize.");
        var totalBytes = checked((long)ZLinkStreamFrameCodec.PrefixSize + messageSize);
        if (totalBytes > int.MaxValue)
            throw new InvalidDataException("STREAM frame exceeds the supported size.");
        if (_length < totalBytes) return false;

        Message header;
        Message payload;
        try
        {
            header = Message.From(input.Slice(ZLinkStreamFrameCodec.PrefixSize, headerSize));
            try
            {
                payload = Message.From(
                    input.Slice(
                        ZLinkStreamFrameCodec.PrefixSize + headerSize,
                        (int)payloadSize));
            }
            catch
            {
                header.Dispose();
                throw;
            }
        }
        catch (ArgumentException exception)
        {
            throw new InvalidDataException("STREAM frame contains an invalid length.", exception);
        }

        Consume((int)totalBytes);
        frame = new ZLinkStreamInboundFrame(header, payload);
        return true;
    }

    internal bool TryGetCompleteFrameSize(out long messageBytes)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        messageBytes = 0;
        if (_length < ZLinkStreamFrameCodec.PrefixSize) return false;

        var input = _buffer.AsSpan(_offset, _length);
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(input[..2]);
        var payloadSize = BinaryPrimitives.ReadUInt32BigEndian(input.Slice(2, 4));
        if (payloadSize > int.MaxValue)
            throw new InvalidDataException("STREAM payload length exceeds the supported size.");

        var messageSize = checked((long)headerSize + payloadSize);
        if (_maxMessageSize > 0 && messageSize > _maxMessageSize)
            throw new InvalidDataException("STREAM frame exceeds MaxMessageSize.");
        var totalBytes = checked((long)ZLinkStreamFrameCodec.PrefixSize + messageSize);
        if (totalBytes > int.MaxValue)
            throw new InvalidDataException("STREAM frame exceeds the supported size.");
        if (_length < totalBytes) return false;

        messageBytes = messageSize;
        return true;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        ArrayPool<byte>.Shared.Return(_buffer);
        _buffer = Array.Empty<byte>();
        _offset = 0;
        _length = 0;
    }

    private void EnsureCapacity(int required)
    {
        if (required <= _buffer.Length - _offset)
            return;

        if (_offset > 0)
        {
            _buffer.AsSpan(_offset, _length).CopyTo(_buffer);
            _offset = 0;
            if (required <= _buffer.Length)
                return;
        }

        var capacity = _buffer.Length;
        while (capacity < required)
        {
            if (capacity > int.MaxValue / 2)
                throw new InvalidDataException("STREAM frame exceeds the supported size.");
            capacity *= 2;
        }

        var replacement = ArrayPool<byte>.Shared.Rent(capacity);
        _buffer.AsSpan(_offset, _length).CopyTo(replacement);
        ArrayPool<byte>.Shared.Return(_buffer);
        _buffer = replacement;
        _offset = 0;
    }

    private void Consume(int bytes)
    {
        _offset += bytes;
        _length -= bytes;
        if (_length == 0)
        {
            _offset = 0;
            TrimEmptyBuffer();
            return;
        }

        if (_offset >= _buffer.Length / 2)
        {
            _buffer.AsSpan(_offset, _length).CopyTo(_buffer);
            _offset = 0;
        }
    }

    private void TrimEmptyBuffer()
    {
        if (_buffer.Length <= DefaultCapacity) return;
        ArrayPool<byte>.Shared.Return(_buffer);
        _buffer = ArrayPool<byte>.Shared.Rent(DefaultCapacity);
    }
}

internal sealed class ZLinkStreamInboundFrame(Message header, Message payload) : IDisposable
{
    private ZLinkInboundDispatchLease? _dispatchLease;

    internal Message? Header { get; private set; } = header;
    internal Message? Payload { get; private set; } = payload;

    // A frame that was received before the HWM became full keeps its dispatch
    // reservation while it waits for the session queue. This prevents a
    // complete frame from being reclassified as a new receive on retry.
    internal void AttachDispatchLease(ZLinkInboundDispatchLease lease)
    {
        ArgumentNullException.ThrowIfNull(lease);
        if (Interlocked.CompareExchange(ref _dispatchLease, lease, null) is not null)
            throw new InvalidOperationException(
                "A STREAM frame already owns an inbound dispatch lease.");
    }

    internal ZLinkInboundDispatchLease? TakeDispatchLease() =>
        Interlocked.Exchange(ref _dispatchLease, null);

    internal long ByteLength => checked((long)(Header?.Size ?? 0) + (Payload?.Size ?? 0));

    internal void Detach()
    {
        Header = null;
        Payload = null;
    }

    public void Dispose()
    {
        Header?.Dispose();
        Payload?.Dispose();
        Interlocked.Exchange(ref _dispatchLease, null)?.Dispose();
        Header = null;
        Payload = null;
    }
}

internal sealed class ZLinkStreamReceiveState(RoutingId routingId, long maxMessageSize)
    : IDisposable
{
    internal readonly object Gate = new();
    internal readonly RoutingId RoutingId = routingId;
    internal readonly ZLinkStreamReceiveBuffer Buffer =
        new(maxMessageSize);
    internal ZLinkStreamInboundFrame? Pending;
    internal bool Removed;

    public void Dispose()
    {
        lock (Gate)
        {
            if (Removed) return;
            Removed = true;
            Pending?.Dispose();
            Pending = null;
            Buffer.Dispose();
        }
    }
}
