using System.Buffers;
using System.Buffers.Text;
using System.Collections.Concurrent;
using System.Collections.Immutable;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Messaging;

internal sealed class ZLinkMultipartPayloadView(
    Message frame,
    int[] ranges)
{
    private ReadOnlyMemory<byte>? _managedFrame;

    internal int Count => ranges.Length / 2;

    internal ReadOnlySpan<byte> GetSpan(int index)
    {
        EnsureIndex(index);
        return frame.AsReadOnlySpan().Slice(
            ranges[index * 2],
            ranges[index * 2 + 1]);
    }

    internal ReadOnlyMemory<byte> GetMemory(int index)
    {
        EnsureIndex(index);
        var memory = _managedFrame ??= frame.AsReadOnlyMemory();
        return memory.Slice(ranges[index * 2], ranges[index * 2 + 1]);
    }

    internal IReadOnlyList<Message> RetainMessages()
    {
        var result = new Message[Count];
        var created = 0;
        try
        {
            for (; created < result.Length; created++)
                result[created] = Message.From(GetSpan(created));
            return result;
        }
        catch
        {
            for (var index = 0; index < created; index++)
                result[index].Dispose();
            throw;
        }
    }

    private void EnsureIndex(int index)
    {
        if ((uint)index >= (uint)Count)
            throw new ArgumentOutOfRangeException(nameof(index));
    }
}

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

    // Cross-language envelope metadata ("metadata" in the wire JSON, matching
    // the C++/Node codecs). Omitted from the wire when absent; peers that
    // always emit an empty object decode identically.
    [System.Text.Json.Serialization.JsonIgnore(
        Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public Dictionary<string, string>? Metadata { get; init; }
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
    private static readonly ZLinkStateLane CacheLane = new();
    private static ImmutableDictionary<SimpleHeaderKey, HeaderPlan> SimpleHeaderCache =
        ImmutableDictionary<SimpleHeaderKey, HeaderPlan>.Empty;
    private static readonly ConcurrentQueue<SimpleHeaderKey> SimpleHeaderCacheOrder = new();
    private static HeaderCacheEntry[] DecodedHeaderCache = [];

    public static string DefaultContentType => JsonContentType;

    public static IReadOnlyList<Message> EncodeParts(
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        var bodyMessage = EncodeBody(body, bodyType, codecs, out var contentType);
        try
        {
            return ZLinkMessageParts.Create(
                EncodeHeader(header, contentType),
                bodyMessage);
        }
        catch
        {
            bodyMessage.Dispose();
            throw;
        }
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
            && header.Metadata is not { Count: > 0 }
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

        return EncodePlannedHeader(header, GetHeaderPlan(new SimpleHeaderKey(
            header.Kind, header.ChannelName, header.MessageName, header.ContentType)));
    }

    public static Message EncodeBody(object? body, Type? bodyType, ZLinkCodecRegistryBuilder? codecs)
    {
        return EncodeBody(body, bodyType, codecs, out _);
    }

    // The encoded body may outlive one request attempt (for example, a channel
    // reselection). Return the resolved content type with the owned Message so
    // each fresh envelope header describes those same bytes without resolving
    // or serializing the typed value again.
    public static Message EncodeBody(
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs,
        out string contentType)
    {
        if (bodyType == typeof(ZLinkMessage))
        {
            if (body is not ZLinkMessage message)
                throw new InvalidOperationException(
                    $"Envelope body type is ZLinkMessage, but body instance is '{body?.GetType()}'.");

            return message.ToRawMessage(codecs ?? new ZLinkCodecRegistryBuilder(), out contentType);
        }

        var hasSerializer = TryResolveBodySerializer(
            body,
            bodyType,
            codecs,
            out var resolvedContentType,
            out var serializer,
            out var resolutionCompleted);
        contentType = resolutionCompleted
            ? resolvedContentType
            : JsonContentType;
        return EncodeBody(
            body,
            bodyType,
            hasSerializer ? serializer : null);
    }

    private static Message EncodeBody(
        object? body,
        Type? bodyType,
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

        return EncodeSerializedPart(body, bodyType, serializer);
    }

    internal static Message EncodeSerializedPart(
        object? body, Type bodyType, IZLinkMessageSerializer? serializer)
    {
        if (body is null)
            return Message.From(ReadOnlySpan<byte>.Empty);
        if (serializer is not null)
        {
            if (serializer is IZLinkMessagePartSerializer partSerializer)
                return partSerializer.SerializePart(body, bodyType);

            return Message.From(serializer.Serialize(body, bodyType).Bytes.Span);
        }

        return EncodeJsonPart(body, bodyType);
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
        bool validateFlow = true) =>
        DecodeHeader(message.AsReadOnlySpan(), validateFlow);

    private static ZLinkEnvelopeHeader DecodeHeader(
        ReadOnlySpan<byte> bytes,
        bool validateFlow)
    {
        var hash = HashBytes(bytes);
        var cached = FindDecodedHeaderCacheEntry(bytes, hash);
        if (cached is not null)
            return ValidateDecodedFlow(cached, validateFlow);

        ZLinkEnvelopeHeader header;
        try
        {
            header = ReadProtocolHeader(bytes);
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException)
        {
            throw new ZLinkEnvelopeProtocolException(
                InvalidProtocolHeader(),
                $"ZLink envelope header is invalid: {error.Message}");
        }
        ValidateProtocolHeader(header, validateFlow);
        // Correlated, deadline-stamped, or flow-stamped headers are byte-unique
        // per message (correlation ids come from a counter), so caching them
        // guarantees misses while evicting the repeatable command/publish
        // entries this cache exists for. Populated metadata is skipped so the
        // shared cached dictionary instance never carries per-message entries.
        if (header.CorrelationId is null
            && header.Deadline is null
            && header.FlowId is null
            && header.Metadata is not { Count: > 0 })
            AddDecodedHeaderCacheEntry(bytes, hash, header);
        return ValidateDecodedFlow(header, validateFlow);
    }

    private static ZLinkEnvelopeHeader ReadProtocolHeader(ReadOnlySpan<byte> bytes)
    {
        var reader = new Utf8JsonReader(bytes);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("ZLink envelope header must be a JSON object.");

        byte formatMarker = 0;
        ZLinkMessageKind kind = default;
        string? channelName = null, messageName = null, contentType = null;
        string? correlationId = null, topic = null, errorCode = null, errorMessage = null, source = null;
        string? flowId = null;
        ZLinkFlowOrigin? flowOrigin = null;
        DateTimeOffset? deadline = null;
        Dictionary<string, string>? metadata = null;
        var complete = false;
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.EndObject)
            {
                complete = true;
                break;
            }
            if (reader.TokenType != JsonTokenType.PropertyName)
                throw new JsonException("ZLink envelope header property is invalid.");
            var field = ReadHeaderField(ref reader);
            if (!reader.Read()) throw new JsonException("ZLink envelope header value is missing.");
            switch (field)
            {
                case HeaderField.FormatMarker:
                    // The Web JSON profile accepts a quoted byte for this
                    // numeric property; enum fields retain integer-only input.
                    formatMarker = JsonSerializer.Deserialize<byte>(ref reader, ZLinkJsonSerializerOptions.Default);
                    break;
                case HeaderField.Kind: kind = (ZLinkMessageKind)ReadHeaderInteger(ref reader); break;
                case HeaderField.ChannelName: channelName = ReadHeaderString(ref reader); break;
                case HeaderField.MessageName: messageName = ReadHeaderString(ref reader); break;
                case HeaderField.ContentType: contentType = ReadHeaderString(ref reader); break;
                case HeaderField.CorrelationId: correlationId = ReadHeaderString(ref reader); break;
                case HeaderField.Deadline:
                    if (reader.TokenType == JsonTokenType.Null) deadline = null;
                    else if (reader.TokenType == JsonTokenType.String && reader.TryGetDateTimeOffset(out var timestamp))
                        deadline = timestamp;
                    else throw new JsonException("ZLink envelope deadline is invalid.");
                    break;
                case HeaderField.Topic: topic = ReadHeaderString(ref reader); break;
                case HeaderField.ErrorCode: errorCode = ReadHeaderString(ref reader); break;
                case HeaderField.ErrorMessage: errorMessage = ReadHeaderString(ref reader); break;
                case HeaderField.Source: source = ReadHeaderString(ref reader); break;
                case HeaderField.FlowId: flowId = ReadHeaderString(ref reader); break;
                case HeaderField.FlowOrigin:
                    flowOrigin = reader.TokenType == JsonTokenType.Null
                        ? null : (ZLinkFlowOrigin)ReadHeaderInteger(ref reader);
                    break;
                case HeaderField.Metadata: metadata = ReadHeaderMetadata(ref reader); break;
                default: reader.Skip(); break;
            }
        }
        if (!complete || reader.Read()) throw new JsonException("ZLink envelope header is incomplete.");

        return new ZLinkEnvelopeHeader(kind, channelName!, messageName!, contentType!,
            correlationId, deadline, topic, errorCode, errorMessage, source)
        {
            FormatMarker = formatMarker,
            FlowId = flowId,
            FlowOrigin = flowOrigin,
            Metadata = metadata
        };
    }

    private static string? ReadHeaderString(ref Utf8JsonReader reader) =>
        reader.TokenType is JsonTokenType.String or JsonTokenType.Null
            ? reader.GetString()
            : throw new JsonException("ZLink envelope string field is invalid.");

    private static int ReadHeaderInteger(ref Utf8JsonReader reader) =>
        reader.TokenType == JsonTokenType.Number && reader.TryGetInt32(out var value)
            ? value : throw new JsonException("ZLink envelope integer field is invalid.");

    private static Dictionary<string, string>? ReadHeaderMetadata(ref Utf8JsonReader reader)
    {
        if (reader.TokenType == JsonTokenType.Null) return null;
        if (reader.TokenType != JsonTokenType.StartObject)
            throw new JsonException("ZLink envelope metadata must be an object.");
        var metadata = new Dictionary<string, string>();
        while (reader.Read())
        {
            if (reader.TokenType == JsonTokenType.EndObject) return metadata;
            if (reader.TokenType != JsonTokenType.PropertyName)
                throw new JsonException("ZLink envelope metadata key is invalid.");
            var key = reader.GetString()!;
            if (!reader.Read()) throw new JsonException("ZLink envelope metadata value is missing.");
            metadata[key] = ReadHeaderString(ref reader)!;
        }
        throw new JsonException("ZLink envelope metadata is incomplete.");
    }

    private static HeaderField ReadHeaderField(ref Utf8JsonReader reader)
    {
        // Canonical field names fit on the stack. Escaped/long names still use
        // the reader's unescaping and the original ordinal case-insensitive match.
        Span<char> buffer = stackalloc char[14];
        ReadOnlySpan<char> name = reader.ValueSpan.Length <= buffer.Length
            ? buffer[..reader.CopyString(buffer)] : reader.GetString().AsSpan();
        // Length partitions keep the field mapping in one place while retaining
        // the original ordinal case-insensitive property-name contract.
        return name.Length switch
        {
            4 when name.Equals("kind", StringComparison.OrdinalIgnoreCase) => HeaderField.Kind,
            5 when name.Equals("topic", StringComparison.OrdinalIgnoreCase) => HeaderField.Topic,
            6 when name.Equals("source", StringComparison.OrdinalIgnoreCase) => HeaderField.Source,
            6 when name.Equals("flowId", StringComparison.OrdinalIgnoreCase) => HeaderField.FlowId,
            8 when name.Equals("deadline", StringComparison.OrdinalIgnoreCase) => HeaderField.Deadline,
            8 when name.Equals("metadata", StringComparison.OrdinalIgnoreCase) => HeaderField.Metadata,
            9 when name.Equals("errorCode", StringComparison.OrdinalIgnoreCase) => HeaderField.ErrorCode,
            10 when name.Equals("flowOrigin", StringComparison.OrdinalIgnoreCase) => HeaderField.FlowOrigin,
            11 when name.Equals("channelName", StringComparison.OrdinalIgnoreCase) => HeaderField.ChannelName,
            11 when name.Equals("messageName", StringComparison.OrdinalIgnoreCase) => HeaderField.MessageName,
            11 when name.Equals("contentType", StringComparison.OrdinalIgnoreCase) => HeaderField.ContentType,
            12 when name.Equals("formatMarker", StringComparison.OrdinalIgnoreCase) => HeaderField.FormatMarker,
            12 when name.Equals("errorMessage", StringComparison.OrdinalIgnoreCase) => HeaderField.ErrorMessage,
            13 when name.Equals("correlationId", StringComparison.OrdinalIgnoreCase) => HeaderField.CorrelationId,
            _ => HeaderField.Unknown
        };
    }

    private enum HeaderField
    {
        Unknown, FormatMarker, Kind, ChannelName, MessageName, ContentType, CorrelationId,
        Deadline, Topic, ErrorCode, ErrorMessage, Source, FlowId, FlowOrigin, Metadata
    }

    public static ZLinkEnvelopeHeader DecodeHeader(
        IReadOnlyList<Message> parts,
        bool validateFlow = true)
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts[0], validateFlow);
    }

    internal static ZLinkEnvelopeHeader DecodeHeader(
        ZLinkMultipartPayloadView parts,
        bool validateFlow = true)
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts.GetSpan(0), validateFlow);
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

    internal static object? DecodeBody(
        ZLinkMultipartPayloadView parts,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs)
    {
        EnsurePart(parts, 1, "body");
        var body = parts.GetSpan(1);
        IZLinkMessageSerializer? customSerializer = null;
        if (!contentType.Equals(JsonContentType, StringComparison.OrdinalIgnoreCase)
            && (codecs is null
                || !codecs.TryGetSerializer(contentType, out customSerializer)))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"No payload serializer is registered for received content type '{contentType}'.");

        if (bodyType == typeof(Message))
            return Message.From(body);
        if (bodyType == typeof(ZLinkMessage))
            return ZLinkMessage.FromEncoded(
                contentType,
                parts.GetMemory(1),
                codecs ?? new ZLinkCodecRegistryBuilder());
        if (bodyType == typeof(ReadOnlyMemory<byte>))
            return parts.GetMemory(1);
        if (body.IsEmpty)
            return bodyType.IsValueType
                ? Activator.CreateInstance(bodyType)
                : null;
        if (customSerializer is not null)
        {
            if (customSerializer is IZLinkMessageSpanDeserializer spanDeserializer)
                return spanDeserializer.Deserialize(body, bodyType);
            return customSerializer.Deserialize(
                ZLinkEncodedPayload.FromOwned(parts.GetMemory(1)),
                bodyType);
        }
        return ZLinkFrameworkJsonPayloadCodec.Deserialize(body, bodyType);
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

    private static void EnsurePart(
        ZLinkMultipartPayloadView parts,
        int index,
        string name)
    {
        if (parts.Count <= index)
            throw new ZLinkEnvelopeProtocolException(
                InvalidProtocolHeader(),
                $"ZLink envelope {name} part is missing.");
    }

    private static bool IsSimpleHeader(ZLinkEnvelopeHeader header)
    {
        return header.CorrelationId is null
               && header.Deadline is null
               && header.Topic is null
               && header.ErrorCode is null
               && header.ErrorMessage is null
               && header.Source is null
               && header.Metadata is not { Count: > 0 }
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

    private static byte[] GetSimpleHeaderBytes(SimpleHeaderKey key) => GetHeaderPlan(key).Bytes;

    private static HeaderPlan GetHeaderPlan(SimpleHeaderKey key)
    {
        // Message and channel names are application input. Keep a bounded
        // replacement cache so hot keys remain cheap after arbitrary keys
        // have filled the cache.
        if (Volatile.Read(ref SimpleHeaderCache).TryGetValue(key, out var hit))
            return hit;
        return AddOnMiss(key);

        // Keep the mutation closure on the miss path; a warm lookup does not
        // allocate an owner-turn callback merely to return immutable bytes.
        static HeaderPlan AddOnMiss(SimpleHeaderKey key) =>
            AwaitStateLane(CacheLane.RunAsync(() =>
            {
                var cache = SimpleHeaderCache;
                if (cache.TryGetValue(key, out var cached))
                    return cached;

                while (cache.Count >= MaximumSimpleHeaderCacheEntries
                       && SimpleHeaderCacheOrder.TryDequeue(out var evicted))
                    cache = cache.Remove(evicted);

                var bytes = EncodeSimpleHeaderBytes(key);
                ReadOnlySpan<byte> dynamicField = ",\"correlationId\":"u8;
                var dynamicOffset = bytes.AsSpan().IndexOf(dynamicField) + dynamicField.Length;
                var encoded = new HeaderPlan(bytes, dynamicOffset);
                SimpleHeaderCacheOrder.Enqueue(key);
                Volatile.Write(ref SimpleHeaderCache, cache.Add(key, encoded));
                return encoded;
            }));
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

    private static Message EncodePlannedHeader(ZLinkEnvelopeHeader header, HeaderPlan plan)
    {
        var length = WritePlannedHeader(Span<byte>.Empty, header, plan);
        var result = new Message(length);
        try
        {
            if (WritePlannedHeader(result.AsSpan(), header, plan) != length)
                throw new InvalidOperationException("ZLink envelope changed while being encoded.");
            return result;
        }
        catch
        {
            result.Dispose();
            throw;
        }
    }

    private static int WritePlannedHeader(
        Span<byte> destination, ZLinkEnvelopeHeader header, HeaderPlan plan)
    {
        var written = 0;
        WriteHeaderToken(plan.Bytes.AsSpan(0, plan.DynamicOffset), destination, ref written);
        WriteHeaderString(header.CorrelationId, destination, ref written);
        WriteHeaderToken(",\"deadline\":"u8, destination, ref written);
        Span<byte> deadline = stackalloc byte[35];
        WriteHeaderToken(FormatHeaderDeadline(header.Deadline, deadline), destination, ref written);
        WriteHeaderToken(",\"topic\":"u8, destination, ref written);
        WriteHeaderString(header.Topic, destination, ref written);
        WriteHeaderToken(",\"errorCode\":"u8, destination, ref written);
        WriteHeaderString(header.ErrorCode, destination, ref written);
        WriteHeaderToken(",\"errorMessage\":"u8, destination, ref written);
        WriteHeaderString(header.ErrorMessage, destination, ref written);
        WriteHeaderToken(",\"source\":"u8, destination, ref written);
        WriteHeaderString(header.Source, destination, ref written);
        WriteHeaderToken(",\"flowId\":"u8, destination, ref written);
        WriteHeaderString(header.FlowId, destination, ref written);
        WriteHeaderToken(",\"flowOrigin\":"u8, destination, ref written);
        if (header.FlowOrigin is { } origin)
        {
            Span<byte> number = stackalloc byte[11];
            Utf8Formatter.TryFormat((int)origin, number, out var count);
            WriteHeaderToken(number[..count], destination, ref written);
        }
        else
            WriteHeaderToken("null"u8, destination, ref written);

        if (header.Metadata is { } metadata)
        {
            WriteHeaderToken(",\"metadata\":{"u8, destination, ref written);
            var first = true;
            foreach (var entry in metadata)
            {
                if (!first) WriteHeaderToken(","u8, destination, ref written);
                first = false;
                WriteHeaderString(entry.Key, destination, ref written);
                WriteHeaderToken(":"u8, destination, ref written);
                WriteHeaderString(entry.Value, destination, ref written);
            }
            WriteHeaderToken("}"u8, destination, ref written);
        }
        WriteHeaderToken("}"u8, destination, ref written);
        return written;
    }

    private static void WriteHeaderToken(
        ReadOnlySpan<byte> token, Span<byte> destination, ref int written)
    {
        if (!destination.IsEmpty) token.CopyTo(destination[written..]);
        written = checked(written + token.Length);
    }

    private static void WriteHeaderString(string? value, Span<byte> destination, ref int written)
    {
        if (value is null)
        {
            WriteHeaderToken("null"u8, destination, ref written);
            return;
        }

        WriteHeaderToken("\""u8, destination, ref written);
        Span<char> scalar = stackalloc char[2];
        Span<char> escaped = stackalloc char[12];
        foreach (var rune in value.EnumerateRunes())
        {
            if (JavaScriptEncoder.Default.WillEncode(rune.Value))
            {
                var scalarLength = rune.EncodeToUtf16(scalar);
                JavaScriptEncoder.Default.Encode(scalar[..scalarLength], escaped,
                    out _, out var escapedLength);
                if (!destination.IsEmpty)
                    Encoding.UTF8.GetBytes(escaped[..escapedLength], destination[written..]);
                written = checked(written + Encoding.UTF8.GetByteCount(escaped[..escapedLength]));
            }
            else
            {
                if (!destination.IsEmpty) rune.EncodeToUtf8(destination[written..]);
                written = checked(written + rune.Utf8SequenceLength);
            }
        }
        WriteHeaderToken("\""u8, destination, ref written);
    }

    private static ReadOnlySpan<byte> FormatHeaderDeadline(DateTimeOffset? value, Span<byte> buffer)
    {
        if (value is null) return "null"u8;

        buffer[0] = (byte)'"';
        Utf8Formatter.TryFormat(value.Value, buffer[1..], out var length, new StandardFormat('O'));
        // System.Text.Json uses the round-trip timestamp with trailing zero
        // fractions removed, while retaining the DateTimeOffset's UTC offset.
        var end = 27;
        while (buffer[end] == (byte)'0') end--;
        if (buffer[end] == (byte)'.') end--;
        buffer.Slice(28, length - 27).CopyTo(buffer[(end + 1)..]);
        length = end + 1 + length - 27;
        buffer[length] = (byte)'"';
        return buffer[..(length + 1)];
    }

    private sealed record HeaderPlan(byte[] Bytes, int DynamicOffset);

    private static void AddDecodedHeaderCacheEntry(
        ReadOnlySpan<byte> bytes,
        ulong hash,
        ZLinkEnvelopeHeader header)
    {
        if (bytes.Length > 1024) return;
        var copy = bytes.ToArray();
        AddDecodedHeaderCacheEntry(copy, hash, header);
    }

    private static ZLinkEnvelopeHeader? FindDecodedHeaderCacheEntry(
        ReadOnlySpan<byte> bytes,
        ulong hash)
    {
        foreach (var entry in Volatile.Read(ref DecodedHeaderCache))
        {
            if (entry.Hash == hash && entry.Bytes.AsSpan().SequenceEqual(bytes))
                return entry.Header;
        }
        return null;
    }

    private static void AddDecodedHeaderCacheEntry(
        byte[] copy,
        ulong hash,
        ZLinkEnvelopeHeader header) =>
        AwaitStateLane(CacheLane.RunAsync(() =>
        {
            var cache = DecodedHeaderCache;
            foreach (var entry in cache)
            {
                if (entry.Hash == hash && entry.Bytes.AsSpan().SequenceEqual(copy))
                    return;
            }

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
        }));

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

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
