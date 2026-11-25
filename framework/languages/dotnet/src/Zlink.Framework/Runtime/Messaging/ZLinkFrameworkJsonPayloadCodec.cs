using System.Buffers;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkFrameworkJsonPayloadCodec
{
    internal static IZLinkMessageSerializer ReceiveSerializer { get; } =
        new JsonMessageSerializer();

    internal static T? Deserialize<T>(ReadOnlySpan<byte> payload)
    {
        var type =
            (JsonTypeInfo<T>)ZLinkJsonSerializerOptions.FrameworkPayload.GetTypeInfo(typeof(T));
        try
        {
            ZLinkFrameworkJsonDocument.ValidateDocument(payload, type);
            return JsonSerializer.Deserialize(payload, type);
        }
        catch (JsonException error) when (IsProfileFailure(error))
        {
            throw DescribeDecodeFailure(type.Type, error, payload);
        }
    }

    internal static object? Deserialize(ReadOnlySpan<byte> payload, Type type)
    {
        var typeInfo = ZLinkJsonSerializerOptions.FrameworkPayload.GetTypeInfo(type);
        try
        {
            ZLinkFrameworkJsonDocument.ValidateDocument(payload, typeInfo);
            return JsonSerializer.Deserialize(payload, typeInfo);
        }
        catch (JsonException error) when (IsProfileFailure(error))
        {
            throw DescribeDecodeFailure(type, error, payload);
        }
    }

    internal static byte[] Serialize<T>(T value)
    {
        try
        {
            return JsonSerializer.SerializeToUtf8Bytes(
                value,
                ZLinkJsonSerializerOptions.FrameworkPayload
            );
        }
        catch (JsonException error) when (IsProfileFailure(error))
        {
            throw DescribeFailure(typeof(T), error);
        }
    }

    internal static byte[] Serialize(object? value, Type type)
    {
        try
        {
            return JsonSerializer.SerializeToUtf8Bytes(
                value,
                type,
                ZLinkJsonSerializerOptions.FrameworkPayload
            );
        }
        catch (JsonException error) when (IsProfileFailure(error))
        {
            throw DescribeFailure(type, error);
        }
    }

    internal static Message SerializePart(object? value, Type type)
    {
        using var buffer = SerializePooled(value, type);
        return Message.From(buffer.WrittenSpan);
    }

    internal static PooledJsonBufferWriter SerializePooled(object? value, Type type)
    {
        var options = ZLinkJsonSerializerOptions.FrameworkPayload;
        var buffer = new PooledJsonBufferWriter();
        try
        {
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
            return buffer;
        }
        catch (JsonException error) when (IsProfileFailure(error))
        {
            buffer.Dispose();
            throw DescribeFailure(type, error);
        }
        catch
        {
            buffer.Dispose();
            throw;
        }
    }

    private static bool IsProfileFailure(JsonException error) =>
        error.Data.Contains(ZLinkJsonSerializerOptions.DiagnosticTokenOffset);

    private static JsonException DescribeDecodeFailure(
        Type type,
        JsonException error,
        ReadOnlySpan<byte> payload
    ) =>
        DescribeFailure(
            type,
            error,
            error.Data[ZLinkJsonSerializerOptions.DiagnosticTokenOffset] is long tokenOffset
            && tokenOffset >= 0
                ? ZLinkFrameworkJsonDocument.PropertyPathAt(payload, tokenOffset)
                : null
        );

    private static JsonException DescribeFailure(
        Type type,
        JsonException error,
        string? path = null
    ) =>
        new(
            $"Message '{type.Name}' ({path ?? error.Path ?? "$"}): {error.Message}",
            path ?? error.Path,
            error.LineNumber,
            error.BytePositionInLine,
            error.InnerException
        );

    internal sealed class PooledJsonBufferWriter : IBufferWriter<byte>, IDisposable
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
