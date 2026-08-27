using System.Buffers;
using System.Buffers.Binary;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamReceiveBuffer : IDisposable
{
    private const int DefaultCapacity = 4096;
    private readonly long _maxMessageSize;
    private readonly Queue<PayloadOwnerSegment> _payloadOwnerSegments = new();
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
        AppendCore(bytes, payloadOwner: null);
    }

    internal void Append(ReadOnlySpan<byte> bytes, IDisposable payloadOwner)
    {
        ArgumentNullException.ThrowIfNull(payloadOwner);
        if (bytes.IsEmpty)
        {
            payloadOwner.Dispose();
            return;
        }
        try
        {
            AppendCore(bytes, payloadOwner);
        }
        catch
        {
            payloadOwner.Dispose();
            throw;
        }
    }

    internal void Append(
        IReadOnlyList<Message> parts,
        IDisposable payloadOwner)
    {
        ArgumentNullException.ThrowIfNull(parts);
        ArgumentNullException.ThrowIfNull(payloadOwner);
        var ownershipTransferred = false;
        try
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var appendedBytes = 0;
            foreach (var part in parts)
                appendedBytes = checked(appendedBytes + part.Size);
            if (appendedBytes == 0)
                return;

            if (appendedBytes > int.MaxValue - _length)
                throw new InvalidDataException(
                    "STREAM frame exceeds the supported size.");
            var required = _length + appendedBytes;
            EnsureCapacity(required);
            var destinationOffset = _offset + _length;
            foreach (var part in parts)
            {
                var bytes = part.AsReadOnlySpan();
                bytes.CopyTo(_buffer.AsSpan(destinationOffset));
                destinationOffset += bytes.Length;
            }

            _length = required;
            _payloadOwnerSegments.Enqueue(
                new PayloadOwnerSegment(
                    appendedBytes,
                    new SharedPayloadOwner(payloadOwner)));
            ownershipTransferred = true;
        }
        finally
        {
            if (!ownershipTransferred)
                payloadOwner.Dispose();
        }
    }

    private void AppendCore(ReadOnlySpan<byte> bytes, IDisposable? payloadOwner)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (bytes.IsEmpty) return;

        if (bytes.Length > int.MaxValue - _length)
            throw new InvalidDataException("STREAM frame exceeds the supported size.");
        var required = _length + bytes.Length;
        EnsureCapacity(required);
        bytes.CopyTo(_buffer.AsSpan(_offset + _length));
        _length = required;
        _payloadOwnerSegments.Enqueue(
            new PayloadOwnerSegment(
                bytes.Length,
                payloadOwner is null ? null : new SharedPayloadOwner(payloadOwner)));
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
            throw new InvalidDataException("EMSGSIZE: STREAM frame exceeds MaxMessageSize.");
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

        var framePayloadOwner = TakeFramePayloadOwner((int)totalBytes);
        Consume((int)totalBytes);
        frame = new ZLinkStreamInboundFrame(header, payload, framePayloadOwner);
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
            throw new InvalidDataException("EMSGSIZE: STREAM frame exceeds MaxMessageSize.");
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
        while (_payloadOwnerSegments.TryDequeue(out var segment))
            segment.PayloadOwner?.Dispose();
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

    private IDisposable? TakeFramePayloadOwner(int bytes)
    {
        List<IDisposable>? retained = null;
        var remaining = bytes;
        while (remaining > 0 && _payloadOwnerSegments.TryPeek(out var segment))
        {
            var consumed = Math.Min(remaining, segment.RemainingBytes);
            if (segment.PayloadOwner is { } payloadOwner)
            {
                retained ??= new List<IDisposable>();
                retained.Add(payloadOwner.Retain());
            }

            segment.RemainingBytes -= consumed;
            remaining -= consumed;
            if (segment.RemainingBytes != 0)
                continue;

            _payloadOwnerSegments.Dequeue();
            segment.PayloadOwner?.Dispose();
        }

        return retained?.Count switch
        {
            null or 0 => null,
            1 => retained[0],
            _ => new CompositePayloadOwner(retained)
        };
    }

    private sealed class PayloadOwnerSegment(
        int remainingBytes,
        SharedPayloadOwner? payloadOwner)
    {
        internal int RemainingBytes = remainingBytes;
        internal readonly SharedPayloadOwner? PayloadOwner = payloadOwner;
    }

    private sealed class SharedPayloadOwner : IDisposable
    {
        private IDisposable? _owner;
        private int _references = 1;

        internal SharedPayloadOwner(IDisposable owner)
        {
            _owner = owner;
        }

        internal IDisposable Retain()
        {
            while (true)
            {
                var current = Volatile.Read(ref _references);
                if (current == 0)
                    throw new ObjectDisposedException(nameof(SharedPayloadOwner));
                if (Interlocked.CompareExchange(
                        ref _references,
                        checked(current + 1),
                        current) == current)
                    return new PayloadOwnerLease(this);
            }
        }

        public void Dispose()
        {
            if (Interlocked.Decrement(ref _references) != 0)
                return;
            Interlocked.Exchange(ref _owner, null)?.Dispose();
        }

        private sealed class PayloadOwnerLease(SharedPayloadOwner owner) : IDisposable
        {
            private SharedPayloadOwner? _owner = owner;

            public void Dispose() =>
                Interlocked.Exchange(ref _owner, null)?.Dispose();
        }
    }

    private sealed class CompositePayloadOwner(List<IDisposable> owners) : IDisposable
    {
        private List<IDisposable>? _owners = owners;

        public void Dispose()
        {
            var ownersToDispose = Interlocked.Exchange(ref _owners, null);
            if (ownersToDispose is null)
                return;
            foreach (var owner in ownersToDispose)
                owner.Dispose();
        }
    }
}

internal sealed class ZLinkStreamInboundFrame(
    Message header,
    Message payload,
    IDisposable? payloadOwner = null) : IDisposable
{
    internal Message? Header { get; private set; } = header;
    internal Message? Payload { get; private set; } = payload;

    internal ZLinkApplicationJobQueueLease? ApplicationJobAdmission { get; set; }

    internal IDisposable? PayloadOwner { get; private set; } = payloadOwner;

    internal long ByteLength => checked((long)(Header?.Size ?? 0) + (Payload?.Size ?? 0));

    internal void Detach()
    {
        Header = null;
        Payload = null;
        ApplicationJobAdmission = null;
        PayloadOwner = null;
    }

    public void Dispose()
    {
        Header?.Dispose();
        Payload?.Dispose();
        Header = null;
        Payload = null;
        ApplicationJobAdmission?.Dispose();
        ApplicationJobAdmission = null;
        PayloadOwner?.Dispose();
        PayloadOwner = null;
    }
}

internal sealed class ZLinkStreamReceiveState(RoutingId routingId, long maxMessageSize)
    : IDisposable
{
    private readonly ZLinkStateLane _lane = new();
    internal readonly RoutingId RoutingId = routingId;
    internal readonly ZLinkStreamReceiveBuffer Buffer =
        new(maxMessageSize);
    internal ZLinkStreamInboundFrame? Pending;
    internal bool Removed;

    internal void Run(Action work) =>
        _lane.RunAsync(work).GetAwaiter().GetResult();

    internal T Run<T>(Func<T> work) =>
        _lane.RunAsync(work).GetAwaiter().GetResult();

    public void Dispose()
    {
        Run(() =>
        {
            if (Removed) return;
            Removed = true;
            Pending?.Dispose();
            Pending = null;
            Buffer.Dispose();
        });
    }
}
