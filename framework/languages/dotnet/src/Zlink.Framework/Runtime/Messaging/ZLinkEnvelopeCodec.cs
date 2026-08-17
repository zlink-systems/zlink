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

    public string? FlowId { get; set; }

    public ZLinkFlowOrigin? FlowOrigin { get; set; }
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
    private const int MaximumSimpleHeaderCacheEntries = 4096;
    private static readonly ConcurrentDictionary<SimpleHeaderKey, byte[]> SimpleHeaderCache = new();
    private static readonly ConcurrentQueue<SimpleHeaderKey> SimpleHeaderCacheOrder = new();
    private static readonly object SimpleHeaderCacheGate = new();
    private static readonly object DecodedHeaderCacheGate = new();
    private static HeaderCacheEntry[] DecodedHeaderCache = [];

    public static string DefaultContentType => JsonContentType;

    public static IReadOnlyList<Message> EncodeParts(
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        if (bodyType == typeof(ZLinkMessage))
        {
            if (body is not ZLinkMessage message)
                throw new InvalidOperationException(
                    $"Envelope body type is ZLinkMessage, but body instance is '{body?.GetType()}'.");

            var encoded = message.Encode(codecs ?? new ZLinkCodecRegistryBuilder());
            return ZLinkMessageParts.Create(
                EncodeHeader(header, encoded.ContentType),
                Message.From(encoded.Payload.Bytes.Span));
        }

        var hasSerializer = TryResolveBodySerializer(
            body,
            bodyType,
            codecs,
            out var contentType,
            out var serializer,
            out var resolutionCompleted);
        if (hasSerializer)
        {
            var headerMessage = EncodeHeader(header, contentType);
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
            EncodeHeader(
                header,
                resolutionCompleted ? contentType : JsonContentType),
            EncodeBody(
                body,
                bodyType,
                codecs,
                resolutionCompleted,
                serializer));
    }

    public static IReadOnlyList<Message> EncodeRawBodyParts(
        ZLinkEnvelopeHeader header,
        Message body)
    {
        return ZLinkMessageParts.Create(EncodeHeader(header), body);
    }

    public static Message EncodeHeader(ZLinkEnvelopeHeader header) =>
        EncodeHeader(header, header.ContentType);

    // contentType overrides header.ContentType so EncodeParts does not need a
    // record clone just to stamp the resolved serializer's content type.
    public static Message EncodeHeader(ZLinkEnvelopeHeader header, string contentType)
    {
        var flow = ZLinkFlowContext.Current;
        var flowId = header.FlowId ?? flow?.FlowId;
        var flowOrigin = header.FlowOrigin ?? flow?.Origin;

        // Hot path: route/request envelopes usually differ only in the body.
        // The cached bytes are rebuilt canonically from the key (marker
        // included), so a simple, valid header needs neither the record clone
        // nor the full validation walk. Correlated kinds are excluded here so
        // their missing-correlation failure still surfaces via the slow path.
        if (flowId is null
            && flowOrigin is null
            && header.CorrelationId is null
            && header.Deadline is null
            && header.Topic is null
            && header.ErrorCode is null
            && header.ErrorMessage is null
            && header.Source is null
            && header.Kind is not (ZLinkMessageKind.Request
                or ZLinkMessageKind.Response
                or ZLinkMessageKind.Error)
            && Enum.IsDefined(header.Kind))
        {
            var key = new SimpleHeaderKey(
                header.Kind,
                header.ChannelName,
                header.MessageName,
                contentType);
            var bytes = GetSimpleHeaderBytes(key);
            return Message.From(bytes);
        }

        header = header with
        {
            FormatMarker = ZlinkStreamFlowId.FormatMarker,
            FlowId = flowId,
            FlowOrigin = flowOrigin,
            ContentType = contentType
        };
        ValidateProtocolHeader(header);
        if (IsSimpleHeader(header))
        {
            var key = new SimpleHeaderKey(
                header.Kind,
                header.ChannelName,
                header.MessageName,
                header.ContentType);
            return Message.From(GetSimpleHeaderBytes(key));
        }

        return EncodeProtocolPart(header);
    }

    public static Message EncodeBody(object? body, Type? bodyType, ZLinkCodecRegistryBuilder? codecs)
    {
        var hasSerializer = TryResolveBodySerializer(
            body,
            bodyType,
            codecs,
            out _,
            out var serializer,
            out var resolutionCompleted);
        return EncodeBody(
            body,
            bodyType,
            codecs,
            resolutionCompleted,
            hasSerializer ? serializer : null);
    }

    private static Message EncodeBody(
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs,
        bool resolutionCompleted,
        IZLinkMessageSerializer? serializer)
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

        if (!resolutionCompleted)
            return EncodeJsonPart(body, bodyType);

        if (serializer is not null)
        {
            if (serializer is IZLinkMessagePartSerializer partSerializer)
                return partSerializer.SerializePart(body, bodyType);

            return Message.From(serializer.Serialize(body, bodyType).Bytes.Span);
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
        return EncodeProtocolPart(value);
    }

    public static ZLinkEnvelopeHeader DecodeHeader(
        Message message,
        bool validateFlow = true)
    {
        var bytes = message.AsReadOnlySpan();
        var hash = HashBytes(bytes);
        var cache = Volatile.Read(ref DecodedHeaderCache);
        foreach (var entry in cache)
        {
            if (entry.Hash == hash && entry.Bytes.AsSpan().SequenceEqual(bytes))
                return ValidateDecodedFlow(entry.Header, validateFlow);
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
        ValidateProtocolHeader(header, validateFlow);
        // Correlated, deadline-stamped, or flow-stamped headers are byte-unique
        // per message (correlation ids come from a counter), so caching them
        // guarantees misses while evicting the repeatable command/publish
        // entries this cache exists for.
        if (header.CorrelationId is null
            && header.Deadline is null
            && header.FlowId is null)
            AddDecodedHeaderCacheEntry(bytes, hash, header);
        return ValidateDecodedFlow(header, validateFlow);
    }

    public static ZLinkEnvelopeHeader DecodeHeader(
        IReadOnlyList<Message> parts,
        bool validateFlow = true)
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts[0], validateFlow);
    }

    internal static ulong MeasureApplicationPayloadBytes(
        IReadOnlyList<Message> parts)
    {
        var firstApplicationPart = 0;
        if (parts.Count != 0 && IsEnvelopeHeader(parts[0]))
            firstApplicationPart = 1;

        var total = 0UL;
        for (var index = firstApplicationPart; index < parts.Count; index++)
        {
            var size = (ulong)Math.Max(parts[index].Size, 0);
            if (size > ulong.MaxValue - total)
                return ulong.MaxValue;
            total += size;
        }
        return total;
    }

    private static bool IsEnvelopeHeader(Message message)
    {
        var bytes = message.AsReadOnlySpan();
        var first = 0;
        while (first < bytes.Length
               && bytes[first] is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
            first++;
        if (first == bytes.Length || bytes[first] != (byte)'{')
            return false;

        try
        {
            _ = DecodeHeader(message);
            return true;
        }
        catch (ZLinkEnvelopeProtocolException)
        {
            return false;
        }
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
                ZLinkEncodedPayload.FromOwned(bodyMessage.AsReadOnlyMemory()),
                bodyType);
        }

        return ZLinkFrameworkJsonPayloadCodec.Deserialize(
            bodyMessage.AsReadOnlySpan(),
            bodyType);
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
        return ZLinkFrameworkJsonPayloadCodec.Serialize(value);
    }

    public static byte[] EncodeJsonBytes(object? value, Type valueType)
    {
        return ZLinkFrameworkJsonPayloadCodec.Serialize(value, valueType);
    }

    public static byte[] EncodeProtocolJsonBytes<T>(T value) =>
        JsonSerializer.SerializeToUtf8Bytes(value, ZLinkJsonSerializerOptions.Default);

    private static Message EncodeProtocolPart<T>(T value) =>
        Message.From(EncodeProtocolJsonBytes(value));

    private static bool TryResolveBodySerializer(
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs,
        out string contentType,
        out IZLinkMessageSerializer? serializer,
        out bool resolutionCompleted)
    {
        contentType = JsonContentType;
        serializer = null;
        resolutionCompleted = false;
        if (body is null || bodyType is null) return false;
        if (bodyType == typeof(Message) || body is Message) return false;
        if (bodyType == typeof(ZLinkMessage) || body is ZLinkMessage) return false;

        resolutionCompleted = true;

        if (codecs is not null
            && codecs.TryResolveSerializer(bodyType, out contentType, out serializer))
            return true;

        if (codecs?.SingleCustomSerializer() is { } custom)
        {
            contentType = custom.ContentType;
            serializer = custom.Serializer;
            return true;
        }

        contentType = JsonContentType;
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

    private static void ValidateProtocolHeader(
        ZLinkEnvelopeHeader header,
        bool validateFlow = true)
    {
        if (!Enum.IsDefined(header.Kind))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope message kind is invalid.");

        if (header.FormatMarker != ZlinkStreamFlowId.FormatMarker)
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope format marker is invalid.");

        if (validateFlow)
        {
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
        }

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

    private static ZLinkEnvelopeHeader ValidateDecodedFlow(
        ZLinkEnvelopeHeader header,
        bool validateFlow)
    {
        if (validateFlow) return header;

        // Spec 27 section 4: Off ingress does not retain observation-only
        // fields where a later forwarder or reply encoder could copy them.
        header.FlowId = null;
        header.FlowOrigin = null;
        return header;
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

    private static byte[] GetSimpleHeaderBytes(SimpleHeaderKey key)
    {
        if (SimpleHeaderCache.TryGetValue(key, out var cached))
            return cached;

        // Message and channel names are application input. Keep a bounded
        // replacement cache so hot keys remain cheap after arbitrary keys
        // have filled the cache.
        lock (SimpleHeaderCacheGate)
        {
            if (SimpleHeaderCache.TryGetValue(key, out cached))
                return cached;

            while (SimpleHeaderCache.Count >= MaximumSimpleHeaderCacheEntries
                   && SimpleHeaderCacheOrder.TryDequeue(out var evicted))
                SimpleHeaderCache.TryRemove(evicted, out _);

            var encoded = EncodeSimpleHeaderBytes(key);
            SimpleHeaderCache[key] = encoded;
            SimpleHeaderCacheOrder.Enqueue(key);
            return encoded;
        }
    }

    private static byte[] EncodeSimpleHeaderBytes(SimpleHeaderKey key) =>
        EncodeProtocolJsonBytes(new ZLinkEnvelopeHeader(
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
        });

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
