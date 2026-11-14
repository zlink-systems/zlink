using System.Buffers;
using System.Text.Json;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkFrameworkJsonPayloadCodec
{
    internal static IZLinkMessageSerializer ReceiveSerializer { get; } =
        new JsonMessageSerializer();

    internal static T? Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        ZLinkFrameworkJsonDocument.ValidateDocument(payload);
        return JsonSerializer.Deserialize<T>(payload, ZLinkJsonSerializerOptions.FrameworkPayload);
    }

    internal static object? Deserialize(ReadOnlySpan<byte> payload, Type type)
    {
        ZLinkFrameworkJsonDocument.ValidateDocument(payload);
        return JsonSerializer.Deserialize(
            payload,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload
        );
    }

    internal static byte[] Serialize<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.FrameworkPayload);

    internal static byte[] Serialize(object? value, Type type) =>
        JsonSerializer.SerializeToUtf8Bytes(
            value,
            type,
            ZLinkJsonSerializerOptions.FrameworkPayload
        );

    internal static Message SerializePart(object? value, Type type)
    {
        var options = ZLinkJsonSerializerOptions.FrameworkPayload;
        using var buffer = new PooledJsonBufferWriter();
        using var writer = new Utf8JsonWriter(
            buffer,
            new JsonWriterOptions
            {
                Encoder = options.Encoder,
                Indented = options.WriteIndented,
                MaxDepth = options.MaxDepth == 0 ? 64 : options.MaxDepth,
                SkipValidation = true,
            }
        );
        JsonSerializer.Serialize(writer, value, type, options);
        writer.Flush();
        return Message.From(buffer.WrittenSpan);
    }

    private sealed class PooledJsonBufferWriter : IBufferWriter<byte>, IDisposable
    {
        private byte[] _buffer = ArrayPool<byte>.Shared.Rent(256);
        private int _written;

        internal ReadOnlySpan<byte> WrittenSpan => _buffer.AsSpan(0, _written);

        public void Advance(int count)
        {
            if ((uint)count > (uint)(_buffer.Length - _written))
                throw new ArgumentOutOfRangeException(nameof(count));
            _written += count;
        }

        public Memory<byte> GetMemory(int sizeHint = 0)
        {
            EnsureCapacity(sizeHint);
            return _buffer.AsMemory(_written);
        }

        public Span<byte> GetSpan(int sizeHint = 0)
        {
            EnsureCapacity(sizeHint);
            return _buffer.AsSpan(_written);
        }

        public void Dispose()
        {
            ReturnBuffer();
            _buffer = [];
            _written = 0;
        }

        private void EnsureCapacity(int sizeHint)
        {
            if (sizeHint < 0)
                throw new ArgumentOutOfRangeException(nameof(sizeHint));
            var required = checked(_written + Math.Max(sizeHint, 1));
            if (required <= _buffer.Length)
                return;

            var expanded = ArrayPool<byte>.Shared.Rent(required);
            WrittenSpan.CopyTo(expanded);
            ReturnBuffer();
            _buffer = expanded;
        }

        private void ReturnBuffer()
        {
            _buffer.AsSpan(0, _written).Clear();
            ArrayPool<byte>.Shared.Return(_buffer);
        }
    }

    private sealed class JsonMessageSerializer
        : IZLinkMessageSerializer,
            IZLinkMessageSpanDeserializer
    {
        ZLinkEncodedPayload IZLinkMessageSerializer.Serialize(object value, Type type) =>
            ZLinkEncodedPayload.FromOwned(Serialize(value, type));

        object? IZLinkMessageSerializer.Deserialize(ZLinkEncodedPayload payload, Type type) =>
            Deserialize(payload.Bytes.Span, type);

        object? IZLinkMessageSpanDeserializer.Deserialize(ReadOnlySpan<byte> payload, Type type) =>
            Deserialize(payload, type);
    }
}
