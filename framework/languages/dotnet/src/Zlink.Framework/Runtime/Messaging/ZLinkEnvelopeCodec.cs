using System.Collections.Concurrent;
using System.Text.Json;
using System.Threading;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Messaging;

internal enum ZLinkMessageKind
{
    Request = 1,
    Response = 2,
    Command = 3,
    Publish = 4,
    Error = 5
}

internal sealed record ZLinkEnvelopeHeader(
    ZLinkMessageKind Kind,
    string ChannelName,
    string MessageName,
    string ContentType,
    string? CorrelationId,
    DateTimeOffset? Deadline,
    string? Topic,
    string? ErrorCode,
    string? ErrorMessage,
    string? Source = null)
{
    [System.Text.Json.Serialization.JsonPropertyOrder(-100)]
    public byte FormatMarker { get; init; }

    public string? FlowId { get; init; }

    public ZLinkFlowOrigin? FlowOrigin { get; init; }
}

internal sealed class ZLinkEnvelopeProtocolException(
    ZLinkEnvelopeHeader header,
    string message) : InvalidOperationException(message)
{
    public ZLinkEnvelopeHeader Header { get; } = header;
}

internal static class ZLinkEnvelopeCodec
{
    private const string JsonContentType = "application/json";
    private static readonly ConcurrentDictionary<SimpleHeaderKey, byte[]> SimpleHeaderCache = new();
    private static readonly object DecodedHeaderCacheGate = new();
    private static HeaderCacheEntry[] DecodedHeaderCache = [];

    public static string DefaultContentType => JsonContentType;

    public static IReadOnlyList<Message> EncodeParts(
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        if (TryResolveBodySerializer(body, bodyType, codecs, out var contentType, out var serializer))
        {
            var headerMessage = EncodeHeader(header with { ContentType = contentType });
            try
            {
                return ZLinkMessageParts.Create(
                    headerMessage,
                    EncodeBodyWithSerializer(body!, bodyType!, serializer!));
            }
            catch
            {
                headerMessage.Dispose();
                throw;
            }
        }

        return ZLinkMessageParts.Create(
            EncodeHeader(header with { ContentType = ResolveContentType(body, bodyType, codecs) }),
            EncodeBody(body, bodyType, codecs));
    }

    public static IReadOnlyList<Message> EncodeRawBodyParts(
        ZLinkEnvelopeHeader header,
        Message body)
    {
        return ZLinkMessageParts.Create(EncodeHeader(header), body);
    }

    public static Message EncodeHeader(ZLinkEnvelopeHeader header)
    {
        var flow = ZLinkFlowContext.Current;
        header = header with
        {
            FormatMarker = ZlinkStreamFlowId.FormatMarker,
            FlowId = header.FlowId ?? flow?.FlowId,
            FlowOrigin = header.FlowOrigin ?? flow?.Origin
        };
        ValidateProtocolHeader(header);
        if (IsSimpleHeader(header))
        {
            // Hot path: route/request envelopes usually differ only in the body.
            // Reusing immutable header bytes avoids JSON serialization and UTF-8
            // allocation for every request.
            var bytes = SimpleHeaderCache.GetOrAdd(
                new SimpleHeaderKey(header.Kind, header.ChannelName, header.MessageName, header.ContentType),
                static key => EncodeJsonBytes(new ZLinkEnvelopeHeader(
                    key.Kind,
                    key.ChannelName,
                    key.MessageName,
                    key.ContentType,
                    null,
                    null,
                    null,
                    null,
                    null)
                {
                    FormatMarker = ZlinkStreamFlowId.FormatMarker
                }));
            return Message.From(bytes);
        }

        return EncodeJsonPart(header);
    }

    public static Message EncodeBody(object? body, Type? bodyType, ZLinkCodecRegistryBuilder? codecs)
    {
        if (bodyType is null || body is null) return Message.From(ReadOnlySpan<byte>.Empty);

        if (bodyType == typeof(Message))
        {
            if (body is not Message message)
                throw new InvalidOperationException(
                    $"Envelope body type is Message, but body instance is '{body.GetType()}'.");

            return Message.From(message);
        }

        if (bodyType == typeof(ZLinkMessage))
        {
            if (body is not ZLinkMessage message)
                throw new InvalidOperationException(
                    $"Envelope body type is ZLinkMessage, but body instance is '{body.GetType()}'.");

            return Message.From(message.Encode(codecs ?? new ZLinkCodecRegistryBuilder()).Payload.Bytes.Span);
        }

        if (codecs is not null
            && codecs.TryResolveSerializer(bodyType, out _, out var serializer))
        {
            if (serializer is IZLinkMessagePartSerializer partSerializer)
                return partSerializer.SerializePart(body, bodyType);

            return Message.From(serializer.Serialize(body, bodyType).Bytes.Span);
        }

        if (codecs?.SingleCustomSerializer() is { } custom)
        {
            if (custom.Serializer is IZLinkMessagePartSerializer partSerializer)
                return partSerializer.SerializePart(body, bodyType);

            return Message.From(custom.Serializer.Serialize(body, bodyType).Bytes.Span);
        }

        return EncodeJsonPart(body, bodyType);
    }

    private static Message EncodeBodyWithSerializer(
        object body,
        Type bodyType,
        IZLinkMessageSerializer serializer)
    {
        if (serializer is IZLinkMessagePartSerializer partSerializer)
            return partSerializer.SerializePart(body, bodyType);

        return Message.From(serializer.Serialize(body, bodyType).Bytes.Span);
    }

    public static T DecodePart<T>(Message message)
    {
        return JsonSerializer.Deserialize<T>(message.AsReadOnlySpan(), ZLinkJsonSerializerOptions.Default)
               ?? throw new InvalidOperationException($"Invalid {typeof(T).Name} message part.");
    }

    public static Message EncodePart<T>(T value)
    {
        return EncodeJsonPart(value);
    }

    public static ZLinkEnvelopeHeader DecodeHeader(Message message)
    {
        var bytes = message.AsReadOnlySpan();
        var hash = HashBytes(bytes);
        var cache = Volatile.Read(ref DecodedHeaderCache);
        foreach (var entry in cache)
        {
            if (entry.Hash == hash && entry.Bytes.AsSpan().SequenceEqual(bytes))
                return entry.Header;
        }

        ZLinkEnvelopeHeader header;
        try
        {
            header = JsonSerializer.Deserialize<ZLinkEnvelopeHeader>(
                         bytes,
                         ZLinkJsonSerializerOptions.Default)
                     ?? throw new JsonException("ZLink envelope header is null.");
        }
        catch (JsonException error)
        {
            throw new ZLinkEnvelopeProtocolException(
                InvalidProtocolHeader(),
                $"ZLink envelope header is invalid: {error.Message}");
        }
        ValidateProtocolHeader(header);
        AddDecodedHeaderCacheEntry(bytes, hash, header);
        return header;
    }

    public static ZLinkEnvelopeHeader DecodeHeader(IReadOnlyList<Message> parts)
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts[0]);
    }

    public static ZLinkEnvelopeProtocolException MissingHeader() => new(
        InvalidProtocolHeader(),
        "ZLink envelope header is missing.");

    public static object? DecodeBody(IReadOnlyList<Message> parts, Type bodyType)
    {
        return DecodeBody(parts, bodyType, null);
    }

    public static object? DecodeBody(
        IReadOnlyList<Message> parts,
        Type bodyType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        EnsurePart(parts, 0, "header");
        return DecodeBody(parts, bodyType, DecodeHeader(parts[0]).ContentType, codecs);
    }

    public static object? DecodeBody(
        IReadOnlyList<Message> parts,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        EnsurePart(parts, 1, "body");
        return DecodeBody(parts[1], bodyType, contentType, codecs);
    }

    public static object? DecodeBody(
        Message bodyMessage,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        IZLinkMessageSerializer? customSerializer = null;
        if (!contentType.Equals(JsonContentType, StringComparison.OrdinalIgnoreCase)
            && (codecs is null || !codecs.TryGetSerializer(contentType, out customSerializer)))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"No payload serializer is registered for received content type '{contentType}'.");

        if (bodyType == typeof(Message)) return bodyMessage;

        if (bodyType == typeof(ZLinkMessage))
            return ZLinkMessage.FromEnvelopePayload(contentType, bodyMessage,
                codecs ?? new ZLinkCodecRegistryBuilder());

        if (bodyType == typeof(ReadOnlyMemory<byte>)) return bodyMessage.AsReadOnlyMemory();

        if (bodyMessage.Size == 0)
            return bodyType.IsValueType
                ? Activator.CreateInstance(bodyType)
                : null;

        if (customSerializer is not null)
        {
            // Hot path: span deserializers parse directly from the native message
            // buffer. Avoid routing this through ZLinkEncodedPayload unless the
            // codec only exposes the owned-memory contract.
            if (customSerializer is IZLinkMessageSpanDeserializer spanDeserializer)
                return spanDeserializer.Deserialize(bodyMessage.AsReadOnlySpan(), bodyType);

            return customSerializer.Deserialize(
                ZLinkEncodedPayload.From(bodyMessage.AsReadOnlyMemory()),
                bodyType);
        }

        return JsonSerializer.Deserialize(
            bodyMessage.AsReadOnlySpan(),
            bodyType,
            ZLinkJsonSerializerOptions.Default);
    }

    public static Message EncodeJsonPart<T>(T value)
    {
        return Message.From(EncodeJsonBytes(value));
    }

    public static Message EncodeJsonPart(object? value, Type valueType)
    {
        return Message.From(EncodeJsonBytes(value, valueType));
    }

    public static byte[] EncodeJsonBytes<T>(T value)
    {
        return JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.Default);
    }

    public static byte[] EncodeJsonBytes(object? value, Type valueType)
    {
        return JsonSerializer.SerializeToUtf8Bytes(value, valueType, ZLinkJsonSerializerOptions.Default);
    }

    internal static string ResolveContentType(object? body, Type? bodyType, ZLinkCodecRegistryBuilder? codecs)
    {
        if (body is null || bodyType is null) return JsonContentType;

        if (bodyType == typeof(Message) || body is Message) return JsonContentType;

        if (bodyType == typeof(ZLinkMessage) && body is ZLinkMessage message)
        {
            var encoded = message.Encode(codecs ?? new ZLinkCodecRegistryBuilder());
            return encoded.ContentType;
        }

        if (codecs is not null
            && codecs.TryResolveSerializer(bodyType, out var contentType, out _))
            return contentType;

        if (codecs?.SingleCustomSerializer() is { } custom) return custom.ContentType;

        return JsonContentType;
    }

    private static bool TryResolveBodySerializer(
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs,
        out string contentType,
        out IZLinkMessageSerializer? serializer)
    {
        contentType = JsonContentType;
        serializer = null;
        if (body is null || bodyType is null) return false;
        if (bodyType == typeof(Message) || body is Message) return false;
        if (bodyType == typeof(ZLinkMessage) || body is ZLinkMessage) return false;

        if (codecs is not null
            && codecs.TryResolveSerializer(bodyType, out contentType, out serializer))
            return true;

        if (codecs?.SingleCustomSerializer() is { } custom)
        {
            contentType = custom.ContentType;
            serializer = custom.Serializer;
            return true;
        }

        return false;
    }

    private static void EnsurePart(IReadOnlyList<Message> parts, int index, string name)
    {
        if (parts.Count <= index) throw new InvalidOperationException($"ZLink envelope {name} part is missing.");
    }

    private static bool IsSimpleHeader(ZLinkEnvelopeHeader header)
    {
        return header.CorrelationId is null
               && header.Deadline is null
               && header.Topic is null
               && header.ErrorCode is null
               && header.ErrorMessage is null
               && header.Source is null
               && header.FlowId is null
               && header.FlowOrigin is null;
    }

    private static void ValidateProtocolHeader(ZLinkEnvelopeHeader header)
    {
        if (!Enum.IsDefined(header.Kind))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope message kind is invalid.");

        if (header.FormatMarker != ZlinkStreamFlowId.FormatMarker)
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope format marker is invalid.");

        var hasFlowId = header.FlowId is not null;
        var hasFlowOrigin = header.FlowOrigin is not null;
        if (hasFlowId != hasFlowOrigin)
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope flow id and origin must be present together.");

        if (hasFlowId && !ZlinkStreamFlowId.IsValid(header.FlowId))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope flow id must be UUIDv7.");

        if (header.FlowOrigin is { } origin && !Enum.IsDefined(origin))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope flow origin is invalid.");

        var isReplyCorrelated = header.Kind is ZLinkMessageKind.Request
            or ZLinkMessageKind.Response
            or ZLinkMessageKind.Error;
        if (isReplyCorrelated && string.IsNullOrWhiteSpace(header.CorrelationId))
            throw new ZLinkEnvelopeProtocolException(
                header,
                $"ZLink {header.Kind} envelope requires a correlation id.");

        if (header.Kind == ZLinkMessageKind.Error)
        {
            if (string.IsNullOrWhiteSpace(header.ErrorCode))
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    "ZLink Error envelope requires a non-empty error code.");
        }
        else if (header.ErrorCode is not null || header.ErrorMessage is not null)
        {
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope error fields are valid only for Error messages.");
        }
    }

    private static ZLinkEnvelopeHeader InvalidProtocolHeader() => new(
        ZLinkMessageKind.Command,
        string.Empty,
        string.Empty,
        DefaultContentType,
        null,
        null,
        null,
        null,
        null);

    public static (string? FlowId, ZLinkFlowOrigin? FlowOrigin) ValidFlow(
        ZLinkEnvelopeHeader header)
    {
        if (header.FlowId is null
            || header.FlowOrigin is not { } origin
            || !ZlinkStreamFlowId.IsValid(header.FlowId)
            || !Enum.IsDefined(origin))
            return (null, null);

        return (header.FlowId, origin);
    }

    public static bool CanCorrelateReply(ZLinkEnvelopeHeader header) =>
        !string.IsNullOrWhiteSpace(header.CorrelationId);

    public static string ProtocolErrorMessageName(ZLinkEnvelopeHeader header) =>
        string.IsNullOrWhiteSpace(header.MessageName)
            ? "$zlink.protocol-error"
            : header.MessageName;

    public static void ValidateDispatchHeader(ZLinkEnvelopeHeader header)
    {
        if (string.IsNullOrWhiteSpace(header.ChannelName))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope channel name is missing.");
        if (string.IsNullOrWhiteSpace(header.MessageName))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope message name is missing.");
        if (string.IsNullOrWhiteSpace(header.ContentType))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope content type is missing.");
    }

    private readonly record struct SimpleHeaderKey(
        ZLinkMessageKind Kind,
        string ChannelName,
        string MessageName,
        string ContentType);

    private static void AddDecodedHeaderCacheEntry(
        ReadOnlySpan<byte> bytes,
        ulong hash,
        ZLinkEnvelopeHeader header)
    {
        if (bytes.Length > 1024) return;

        lock (DecodedHeaderCacheGate)
        {
            var cache = DecodedHeaderCache;
            foreach (var entry in cache)
            {
                if (entry.Hash == hash && entry.Bytes.AsSpan().SequenceEqual(bytes))
                    return;
            }

            var copy = bytes.ToArray();
            var next = cache.Length < 64
                ? new HeaderCacheEntry[cache.Length + 1]
                : new HeaderCacheEntry[cache.Length];
            if (cache.Length == next.Length)
            {
                Array.Copy(cache, 1, next, 0, next.Length - 1);
                next[^1] = new HeaderCacheEntry(copy, hash, header);
            }
            else
            {
                Array.Copy(cache, next, cache.Length);
                next[^1] = new HeaderCacheEntry(copy, hash, header);
            }

            Volatile.Write(ref DecodedHeaderCache, next);
        }
    }

    private static ulong HashBytes(ReadOnlySpan<byte> bytes)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;
        var hash = offset;
        foreach (var value in bytes)
        {
            hash ^= value;
            hash *= prime;
        }

        return hash;
    }

    private readonly record struct HeaderCacheEntry(
        byte[] Bytes,
        ulong Hash,
        ZLinkEnvelopeHeader Header);
}
