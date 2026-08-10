using System.Runtime.ExceptionServices;
using System.Text;

namespace Zlink.Framework.Contracts.Messaging;

public sealed partial class ZLinkMessage
{
    private readonly IZLinkMessageCodecResolver? _codecs;
    private readonly Type? _declaredType;
    private readonly ReadOnlyMemory<byte> _payload;
    private readonly object? _value;
    private object? _decodedValue;
    private ExceptionDispatchInfo? _decodeFailure;
    private int _decodeState;

    private ZLinkMessage(object? value, Type? declaredType)
    {
        _value = value;
        _declaredType = declaredType;
    }

    private ZLinkMessage(
        ReadOnlyMemory<byte> payload,
        string? contentType,
        ZlinkStreamCodec? streamCodec,
        IZLinkMessageCodecResolver? codecs)
    {
        _payload = payload;
        ContentType = contentType;
        StreamCodec = streamCodec;
        _codecs = codecs;
    }

    internal EncodedZLinkMessage Encode(IZLinkMessageCodecRegistry codecs)
    {
        if (_declaredType is not null)
        {
            var resolution = ResolvePayloadSerializer(_declaredType, codecs);
            return new EncodedZLinkMessage(
                resolution.ContentType,
                EncodeValue(_value, _declaredType, resolution.Serializer));
        }

        return new EncodedZLinkMessage(
            ContentType ?? DefaultContentType,
            ZLinkEncodedPayload.FromOwned(_payload));
    }

    internal ZLinkMessage Snapshot(IZLinkMessageCodecRegistry codecs)
    {
        var encoded = Encode(codecs);
        return new ZLinkMessage(
            encoded.Payload.Bytes.ToArray(),
            encoded.ContentType,
            null,
            codecs.Snapshot());
    }

    internal static ZLinkMessage FromEncoded(
        string contentType,
        ReadOnlyMemory<byte> payload,
        IZLinkMessageCodecRegistry codecs)
    {
        return new ZLinkMessage(payload, contentType, null, codecs.Snapshot());
    }

    internal Message ToRawMessage(IZLinkMessageCodecRegistry codecs)
    {
        return Message.From(Encode(codecs).Payload.Bytes.Span);
    }

    internal static ZLinkMessage FromStreamPayload(
        ZlinkStreamCodec codec,
        ReadOnlyMemory<byte> payload,
        IZLinkMessageCodecRegistry codecs)
    {
        var snapshot = codecs.Snapshot();
        var contentType = snapshot.TryResolveStreamContentType(codec, out var resolved)
            ? resolved
            : codec == ZlinkStreamCodec.Json
                ? DefaultContentType
                : null;
        return new ZLinkMessage(payload, contentType, codec, snapshot);
    }

    internal static ZLinkMessage FromEnvelopePayload(
        string contentType,
        Message payload,
        IZLinkMessageCodecRegistry codecs)
    {
        return new ZLinkMessage(
            // AsReadOnlyMemory is the binding boundary snapshot for native
            // storage. Do not copy that managed snapshot a second time.
            payload.AsReadOnlyMemory(),
            contentType,
            null,
            codecs.Snapshot());
    }

    private object? Decode(Type targetType)
    {
        if (targetType == typeof(ReadOnlyMemory<byte>))
            return _payload;

        if (targetType == typeof(byte[]))
            return _payload.ToArray();

        return DecodeTyped(targetType);
    }

    private object? DecodeTyped(Type targetType)
    {
        var wait = new SpinWait();
        while (true)
        {
            var state = Volatile.Read(ref _decodeState);
            if (state == 2)
                return CoerceDecodedValue(
                    Volatile.Read(ref _decodedValue),
                    targetType);

            if (state == 3)
            {
                Volatile.Read(ref _decodeFailure)!.Throw();
                return null;
            }

            if (state == 0
                && Interlocked.CompareExchange(ref _decodeState, 1, 0) == 0)
            {
                try
                {
                    var decoded = DecodeTypedCore(targetType);
                    // Validate the cache owner's requested type before any
                    // waiter can observe a successful state. A serializer
                    // that returns the wrong runtime type is one retained
                    // failure, never a transient success for a wider type.
                    var ownerValue = CoerceDecodedValue(decoded, targetType);
                    Volatile.Write(ref _decodedValue, decoded);
                    Volatile.Write(ref _decodeState, 2);
                    return ownerValue;
                }
                catch (Exception exception)
                {
                    Volatile.Write(
                        ref _decodeFailure,
                        ExceptionDispatchInfo.Capture(exception));
                    Volatile.Write(ref _decodeState, 3);
                    throw;
                }
            }

            // The current cache owner publishes either a value or a failure.
            // Waiters cannot deserialize the same accepted payload again.
            wait.SpinOnce();
        }
    }

    private object? DecodeTypedCore(Type targetType)
    {
        if (StreamCodec == ZlinkStreamCodec.Raw
            && targetType == typeof(string))
            return Encoding.UTF8.GetString(_payload.Span);

        if (_payload.Length == 0)
            return targetType.IsValueType
                ? Activator.CreateInstance(targetType)
                : null;

        if (ContentType is not null
            && _codecs is not null
            && _codecs.TryGetSerializer(ContentType, out var serializer))
            return serializer.Deserialize(
                ZLinkEncodedPayload.FromOwned(_payload),
                targetType);

        if (StreamCodec is { } codec
            && codec != ZlinkStreamCodec.Json)
            throw new InvalidOperationException(
                $"Stream payload uses codec '{codec}', but no matching codec extension is registered.");

        return ZLinkFrameworkJsonPayloadCodec.Deserialize(
            _payload.Span,
            targetType);
    }

    private static object? CoerceDecodedValue(
        object? decoded,
        Type targetType)
    {
        if (decoded is null)
        {
            if (!targetType.IsValueType
                || Nullable.GetUnderlyingType(targetType) is not null)
                return null;
        }
        else if (targetType.IsInstanceOfType(decoded))
        {
            return decoded;
        }

        throw new InvalidCastException(
            $"The retained message value cannot be decoded as '{targetType}'.");
    }

    private static ZLinkEncodedPayload EncodeValue(
        object? value,
        Type declaredType,
        IZLinkMessageSerializer? serializer)
    {
        if (value is null)
            return ZLinkEncodedPayload.FromOwned(ReadOnlyMemory<byte>.Empty);

        if (serializer is not null)
            return serializer.Serialize(value, declaredType);

        return ZLinkEncodedPayload.FromOwned(
            ZLinkFrameworkJsonPayloadCodec.Serialize(value, declaredType));
    }

    private static (string ContentType, IZLinkMessageSerializer? Serializer) ResolvePayloadSerializer(
        Type declaredType,
        IZLinkMessageCodecRegistry codecs)
    {
        if (codecs.TryResolveSerializer(declaredType, out var contentType, out var serializer))
            return (contentType, serializer);

        if (codecs.SingleCustomSerializer() is { } custom)
            return (custom.ContentType, custom.Serializer);

        return (DefaultContentType, null);
    }
}

internal readonly record struct EncodedZLinkMessage(string ContentType, ZLinkEncodedPayload Payload);
