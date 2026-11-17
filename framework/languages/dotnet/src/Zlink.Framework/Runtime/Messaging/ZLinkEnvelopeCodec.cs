using System.Buffers;
using System.Text;
using System.Text.Json;
using Google.Protobuf;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;
using ProtoChannelHeader = Zlink.Framework.Channel.V1.ChannelHeader;
using ProtoChannelKind = Zlink.Framework.Channel.V1.ChannelKind;
using ProtoFlowOrigin = Zlink.Framework.Channel.V1.FlowOrigin;

namespace Zlink.Framework.Runtime.Messaging;

internal readonly struct ZLinkMultipartPayloadView
{
    internal const int InlinePartCount = 2;
    internal const int RangeWidth = 2;
    private readonly Message _frame;
    private readonly (int Offset, int Length) _first;
    private readonly (int Offset, int Length) _second;
    private readonly int[]? _ranges;

    internal ZLinkMultipartPayloadView(Message frame, int[] ranges)
        : this(
            frame,
            ranges.Length / RangeWidth,
            ranges.Length >= RangeWidth ? (ranges[0], ranges[1]) : default,
            ranges.Length >= InlinePartCount * RangeWidth ? (ranges[2], ranges[3]) : default,
            ranges.Length > InlinePartCount * RangeWidth ? ranges : null
        ) { }

    internal ZLinkMultipartPayloadView(
        Message frame,
        int count,
        (int Offset, int Length) first,
        (int Offset, int Length) second,
        int[]? ranges
    )
    {
        _frame = frame;
        Count = count;
        _first = first;
        _second = second;
        _ranges = ranges;
    }

    internal readonly int Count { get; }

    internal readonly ReadOnlySpan<byte> GetSpan(int index)
    {
        var range = GetRange(index);
        return _frame.AsReadOnlySpan().Slice(range.Offset, range.Length);
    }

    internal ReadOnlyMemory<byte> GetMemory(int index)
    {
        return GetSpan(index).ToArray();
    }

    internal readonly IReadOnlyList<Message> RetainMessages()
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

    private readonly (int Offset, int Length) GetRange(int index)
    {
        if ((uint)index >= (uint)Count)
            throw new ArgumentOutOfRangeException(nameof(index));
        return _ranges is { } ranges ? (ranges[index * RangeWidth], ranges[index * RangeWidth + 1])
            : index == 0 ? _first
            : _second;
    }
}

internal enum ZLinkMessageKind
{
    Request = 1,
    Response = 2,
    Command = 3,
    Publish = 4,
    Error = 5,
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
    string? Source = null
)
{
    internal Type? PreparedBodyType { get; init; }
    internal IZLinkMessageSerializer? PreparedSerializer { get; init; }

    internal string? DeadlineText { get; init; }

    public string? FlowId { get; set; }

    public ZLinkFlowOrigin? FlowOrigin { get; set; }

    // Cross-language envelope metadata ("metadata" in the wire JSON, matching
    // the C++/Node codecs). Omitted from the wire when absent; peers that
    // always emit an empty object decode identically.
    [System.Text.Json.Serialization.JsonIgnore(
        Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
    )]
    public IReadOnlyDictionary<string, string>? Metadata { get; init; }
}

internal sealed class ZLinkEnvelopeProtocolException(ZLinkEnvelopeHeader header, string message)
    : InvalidOperationException(message)
{
    public ZLinkEnvelopeHeader Header { get; } = header;
}

internal static class ZLinkEnvelopeCodec
{
    private const int MaximumCorrelationIdBytes = 64;
    private const string JsonContentType = "application/json";
    public static string DefaultContentType => JsonContentType;

    public static IReadOnlyList<Message> EncodeParts(
        ZLinkEnvelopeHeader header,
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs
    )
    {
        var contentType = header.ContentType;
        var bodyMessage =
            body is not null and not Message and not ZLinkMessage
            && header.PreparedBodyType is { } preparedType
                ? EncodeBody(body, preparedType, header.PreparedSerializer)
                : EncodeBody(body, bodyType, codecs, out contentType);
        try
        {
            return ZLinkMessageParts.Create(EncodeHeader(header, contentType), bodyMessage);
        }
        catch
        {
            bodyMessage.Dispose();
            throw;
        }
    }

    public static IReadOnlyList<Message> EncodeRawBodyParts(
        ZLinkEnvelopeHeader header,
        Message body
    )
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
        ValidateProtocolHeader(header, true, validateUnicode: true, flowId, flowOrigin);
        var protobuf = ToProto(header, contentType, flowId, flowOrigin);
        var message = Message.Allocate(protobuf.CalculateSize());
        try
        {
            protobuf.WriteTo(message.AsSpan());
            return message;
        }
        catch
        {
            message.Dispose();
            throw;
        }
    }

    internal static ZLinkEnvelopeHeader PrepareResponse(
        string channelName,
        Type? replyType,
        ZLinkCodecRegistryBuilder codecs
    )
    {
        var contentType = DefaultContentType;
        IZLinkMessageSerializer? serializer = null;
        var typed =
            replyType is not null
            && replyType != typeof(Message)
            && replyType != typeof(ZLinkMessage);
        if (typed && !codecs.TryResolveSerializer(replyType!, out contentType, out serializer!))
            contentType = DefaultContentType;
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Response,
            channelName,
            string.Empty,
            contentType,
            null,
            null,
            null,
            null,
            null
        )
        {
            PreparedBodyType = typed ? replyType : null,
            PreparedSerializer = serializer,
        };
        return header;
    }

    public static Message EncodeBody(
        object? body,
        Type? bodyType,
        ZLinkCodecRegistryBuilder? codecs
    )
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
        out string contentType
    )
    {
        if (bodyType == typeof(ZLinkMessage))
        {
            if (body is not ZLinkMessage message)
                throw new InvalidOperationException(
                    $"Envelope body type is ZLinkMessage, but body instance is '{body?.GetType()}'."
                );

            return message.ToRawMessage(codecs, out contentType);
        }

        var hasSerializer = TryResolveBodySerializer(
            body,
            bodyType,
            codecs,
            out var resolvedContentType,
            out var serializer,
            out var resolutionCompleted
        );
        contentType = resolutionCompleted ? resolvedContentType : JsonContentType;
        return EncodeBody(body, bodyType, hasSerializer ? serializer : null);
    }

    private static Message EncodeBody(
        object? body,
        Type? bodyType,
        IZLinkMessageSerializer? serializer
    )
    {
        if (bodyType is null || body is null)
            return Message.From(ReadOnlySpan<byte>.Empty);

        if (bodyType == typeof(Message))
        {
            if (body is not Message message)
                throw new InvalidOperationException(
                    $"Envelope body type is Message, but body instance is '{body.GetType()}'."
                );

            return Message.From(message);
        }

        return EncodeSerializedPart(body, bodyType, serializer);
    }

    internal static Message EncodeSerializedPart(
        object? body,
        Type bodyType,
        IZLinkMessageSerializer? serializer
    )
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
        return JsonSerializer.Deserialize<T>(
                message.AsReadOnlySpan(),
                ZLinkJsonSerializerOptions.Default
            ) ?? throw new InvalidOperationException($"Invalid {typeof(T).Name} message part.");
    }

    public static Message EncodePart<T>(T value)
    {
        return EncodeProtocolPart(value);
    }

    public static ZLinkEnvelopeHeader DecodeHeader(Message message, bool validateFlow = true) =>
        DecodeHeader(message.AsReadOnlySpan(), validateFlow);

    internal static ZLinkEnvelopeHeader DecodeHeader(ReadOnlySpan<byte> bytes, bool validateFlow)
    {
        ProtoChannelHeader proto;
        try
        {
            proto = ProtoChannelHeader.Parser.ParseFrom(bytes);
        }
        catch (Exception error)
            when (error is Google.Protobuf.InvalidProtocolBufferException or ArgumentException)
        {
            throw new ZLinkEnvelopeProtocolException(
                InvalidProtocolHeader(),
                $"ZLink envelope header is invalid: {error.Message}"
            );
        }

        var header = FromProto(proto, validateFlow, out var metadataHasDuplicate);
        ValidateProtocolHeader(
            header,
            validateFlow,
            validateUnicode: false,
            metadataHasDuplicate: metadataHasDuplicate
        );
        return header;
    }

    private static ProtoChannelHeader ToProto(
        ZLinkEnvelopeHeader header,
        string contentType,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin
    )
    {
        var proto = new ProtoChannelHeader
        {
            Kind = (ProtoChannelKind)(int)header.Kind,
            ChannelName = header.ChannelName,
            MessageName = header.MessageName,
            ContentType = contentType,
        };
        if (header.CorrelationId is not null)
            proto.CorrelationId = header.CorrelationId;
        var deadline = header.DeadlineText ?? FormatHeaderDeadline(header.Deadline);
        if (deadline is not null)
            proto.Deadline = deadline;
        if (header.ErrorCode is not null)
            proto.ErrorCode = header.ErrorCode;
        if (header.ErrorMessage is not null)
            proto.ErrorMessage = header.ErrorMessage;
        if (flowId is not null)
            proto.FlowId = flowId;
        if (flowOrigin is { } origin)
            proto.FlowOrigin = (ProtoFlowOrigin)(int)origin;
        if (header.Source is not null)
            proto.Source = header.Source;
        if (header.Topic is not null)
            proto.Topic = header.Topic;
        if (header.Metadata is { Count: > 0 } metadata)
        {
            foreach (
                var (key, value) in metadata.OrderBy(
                    pair => pair.Key,
                    ZLinkMeshMetadataCodec.Utf8ByteComparer
                )
            )
                proto.Metadata.Add(
                    new Zlink.Framework.Channel.V1.MetadataEntry { Key = key, Value = value }
                );
        }
        return proto;
    }

    private static ZLinkEnvelopeHeader FromProto(
        ProtoChannelHeader proto,
        bool validateFlow,
        out bool metadataHasDuplicate
    )
    {
        var deadlineText = proto.HasDeadline ? proto.Deadline : null;

        var metadata =
            proto.Metadata.Count == 0
                ? null
                : new Dictionary<string, string>(proto.Metadata.Count, StringComparer.Ordinal);
        metadataHasDuplicate = false;
        if (metadata is not null)
            foreach (var entry in proto.Metadata)
                if (!metadata.TryAdd(entry.Key, entry.Value))
                    metadataHasDuplicate = true;

        var header = new ZLinkEnvelopeHeader(
            (ZLinkMessageKind)(int)proto.Kind,
            proto.ChannelName,
            proto.MessageName,
            proto.ContentType,
            proto.HasCorrelationId ? proto.CorrelationId : null,
            null,
            proto.HasTopic ? proto.Topic : null,
            proto.HasErrorCode ? proto.ErrorCode : null,
            proto.HasErrorMessage ? proto.ErrorMessage : null,
            proto.HasSource ? proto.Source : null
        )
        {
            DeadlineText = deadlineText,
            FlowId = validateFlow && proto.HasFlowId ? proto.FlowId : null,
            FlowOrigin =
                validateFlow && proto.HasFlowOrigin ? (ZLinkFlowOrigin)(int)proto.FlowOrigin : null,
            Metadata = metadata,
        };
        return header;
    }

    private static string? FormatHeaderDeadline(DateTimeOffset? value)
    {
        if (value is null)
            return null;
        Span<char> formatted = stackalloc char[33];
        if (
            !value.Value.TryFormat(
                formatted,
                out var written,
                "O",
                System.Globalization.CultureInfo.InvariantCulture
            )
        )
            throw new InvalidOperationException("Could not format the envelope deadline.");
        var result = formatted[..written];
        var suffix = result[^1] == 'Z' ? 1 : 6;
        var offsetStart = result.Length - suffix;
        var dot = result.IndexOf('.');
        if (dot < 0 || dot > offsetStart)
            return new string(result);
        var end = offsetStart;
        while (end > dot + 1 && result[end - 1] == '0')
            end--;
        if (end == dot + 1)
            end = dot;
        Span<char> trimmed = stackalloc char[33];
        result[..end].CopyTo(trimmed);
        result[offsetStart..].CopyTo(trimmed[end..]);
        return new string(trimmed[..(end + result.Length - offsetStart)]);
    }

    public static ZLinkEnvelopeHeader DecodeHeader(
        IReadOnlyList<Message> parts,
        bool validateFlow = true
    )
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts[0], validateFlow);
    }

    internal static ZLinkEnvelopeHeader DecodeHeader(
        ZLinkMultipartPayloadView parts,
        bool validateFlow = true
    )
    {
        EnsurePart(parts, 0, "header");
        return DecodeHeader(parts.GetSpan(0), validateFlow);
    }

    public static ZLinkEnvelopeProtocolException MissingHeader() =>
        new(InvalidProtocolHeader(), "ZLink envelope header is missing.");

    public static object? DecodeBody(IReadOnlyList<Message> parts, Type bodyType)
    {
        return DecodeBody(parts, bodyType, null);
    }

    public static object? DecodeBody(
        IReadOnlyList<Message> parts,
        Type bodyType,
        ZLinkCodecRegistryBuilder? codecs
    )
    {
        EnsurePart(parts, 0, "header");
        return DecodeBody(parts, bodyType, DecodeHeader(parts[0]).ContentType, codecs);
    }

    public static object? DecodeBody(
        IReadOnlyList<Message> parts,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs
    )
    {
        EnsurePart(parts, 1, "body");
        return DecodeBody(parts[1], bodyType, contentType, codecs);
    }

    public static object? DecodeBody(
        Message bodyMessage,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs
    )
    {
        var serializer = ResolveReceivedSerializer(contentType, codecs);

        if (bodyType == typeof(Message))
            return bodyMessage;

        if (bodyType == typeof(ZLinkMessage))
            return ZLinkMessage.FromEnvelopePayload(contentType, bodyMessage, codecs);

        if (bodyType == typeof(ReadOnlyMemory<byte>))
            return bodyMessage.AsReadOnlyMemory();

        // The span reads native storage; keep its owner reachable until decoding ends.
        try
        {
            var body = bodyMessage.AsReadOnlySpan();
            if (body.IsEmpty)
                return bodyType.IsValueType ? Activator.CreateInstance(bodyType) : null;

            if (serializer is IZLinkMessageSpanDeserializer spanDeserializer)
                return spanDeserializer.Deserialize(body, bodyType);

            return serializer.Deserialize(ZLinkEncodedPayload.FromOwned(body.ToArray()), bodyType);
        }
        finally
        {
            GC.KeepAlive(bodyMessage);
        }
    }

    internal static object? DecodeBody(
        ZLinkMultipartPayloadView parts,
        Type bodyType,
        string contentType,
        ZLinkCodecRegistryBuilder? codecs
    )
    {
        EnsurePart(parts, 1, "body");
        var body = parts.GetSpan(1);
        var serializer = ResolveReceivedSerializer(contentType, codecs);

        if (bodyType == typeof(Message))
            return Message.From(body);
        if (bodyType == typeof(ZLinkMessage))
            return ZLinkMessage.FromEncoded(contentType, body.ToArray(), codecs);
        if (bodyType == typeof(ReadOnlyMemory<byte>))
            return (ReadOnlyMemory<byte>)body.ToArray();
        if (body.IsEmpty)
            return bodyType.IsValueType ? Activator.CreateInstance(bodyType) : null;

        if (serializer is IZLinkMessageSpanDeserializer spanDeserializer)
            return spanDeserializer.Deserialize(body, bodyType);

        return serializer.Deserialize(ZLinkEncodedPayload.FromOwned(body.ToArray()), bodyType);
    }

    internal static IZLinkMessageSerializer ResolveReceivedSerializer(
        string? contentType,
        IZLinkMessageCodecResolver? codecs
    )
    {
        var resolver = codecs ?? ZLinkCodecRegistrySnapshot.Empty;
        var resolvedContentType = contentType ?? JsonContentType;
        if (resolver.TryGetSerializer(resolvedContentType, out var serializer))
            return serializer;

        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"No payload serializer is registered for received content type '{resolvedContentType}'."
        );
    }

    public static Message EncodeJsonPart<T>(T value)
    {
        return ZLinkFrameworkJsonPayloadCodec.SerializePart(value, typeof(T));
    }

    public static Message EncodeJsonPart(object? value, Type valueType)
    {
        return ZLinkFrameworkJsonPayloadCodec.SerializePart(value, valueType);
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
        out bool resolutionCompleted
    )
    {
        contentType = JsonContentType;
        serializer = null;
        resolutionCompleted = false;
        if (body is null || bodyType is null)
            return false;
        if (bodyType == typeof(Message) || body is Message)
            return false;
        if (bodyType == typeof(ZLinkMessage) || body is ZLinkMessage)
            return false;

        resolutionCompleted = true;

        if (
            codecs is not null
            && codecs.TryResolveSerializer(bodyType, out contentType, out serializer)
        )
            return true;

        contentType = JsonContentType;
        return false;
    }

    private static void EnsurePart(IReadOnlyList<Message> parts, int index, string name)
    {
        if (parts.Count <= index)
            throw new InvalidOperationException($"ZLink envelope {name} part is missing.");
    }

    private static void EnsurePart(ZLinkMultipartPayloadView parts, int index, string name)
    {
        if (parts.Count <= index)
            throw new ZLinkEnvelopeProtocolException(
                InvalidProtocolHeader(),
                $"ZLink envelope {name} part is missing."
            );
    }

    private static bool IsValidMessageKind(ZLinkMessageKind kind) =>
        kind is >= ZLinkMessageKind.Request and <= ZLinkMessageKind.Error;

    private static bool IsValidFlowOrigin(ZLinkFlowOrigin origin) =>
        origin is >= ZLinkFlowOrigin.Inbound and <= ZLinkFlowOrigin.Lifecycle;

    private static void ValidateProtocolHeader(
        ZLinkEnvelopeHeader header,
        bool validateFlow,
        bool validateUnicode = false,
        string? flowId = null,
        ZLinkFlowOrigin? flowOrigin = null,
        bool metadataHasDuplicate = false
    )
    {
        flowId ??= header.FlowId;
        flowOrigin ??= header.FlowOrigin;
        if (metadataHasDuplicate)
            throw new ZLinkEnvelopeProtocolException(header, "duplicate envelope metadata key");
        ValidateHeaderString(header, header.ChannelName, validateUnicode);
        ValidateHeaderString(header, header.MessageName, validateUnicode);
        ValidateHeaderString(header, header.ContentType, validateUnicode);
        ValidateHeaderString(header, header.CorrelationId, validateUnicode);
        ValidateHeaderString(header, header.DeadlineText, validateUnicode);
        ValidateHeaderString(header, header.Topic, validateUnicode);
        ValidateHeaderString(header, header.ErrorCode, validateUnicode);
        ValidateHeaderString(header, header.ErrorMessage, validateUnicode);
        ValidateHeaderString(header, header.Source, validateUnicode);
        ValidateHeaderString(header, flowId, validateUnicode);
        if (header.Metadata is { Count: > 0 } metadata)
        {
            foreach (var (key, value) in metadata)
            {
                if (string.IsNullOrEmpty(key) || value is null)
                    throw new ZLinkEnvelopeProtocolException(
                        header,
                        "ZLink envelope metadata keys must be non-empty."
                    );
                ValidateHeaderString(header, key, validateUnicode);
                ValidateHeaderString(header, value, validateUnicode);
            }
            var size = 0;
            foreach (var (key, value) in metadata)
            {
                var keyBytes = ZLinkMeshMetadataCodec.StrictUtf8.GetByteCount(key);
                var valueBytes = ZLinkMeshMetadataCodec.StrictUtf8.GetByteCount(value);
                var entryBodySize =
                    CodedOutputStream.ComputeTagSize(1)
                    + CodedOutputStream.ComputeLengthSize(keyBytes)
                    + keyBytes
                    + CodedOutputStream.ComputeTagSize(2)
                    + CodedOutputStream.ComputeLengthSize(valueBytes)
                    + valueBytes;
                size = checked(
                    size
                    + CodedOutputStream.ComputeTagSize(13)
                    + CodedOutputStream.ComputeLengthSize(entryBodySize)
                    + entryBodySize
                );
            }
            if (size > ServiceWireConstants.MetadataBytes)
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    $"ZLink envelope metadata exceeds the {ServiceWireConstants.MetadataBytes}-byte limit."
                );
        }
        if (!IsValidMessageKind(header.Kind))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope message kind is invalid."
            );

        if (header.CorrelationId is not null && !CanCorrelateReply(header))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope correlation id must be 1..64 ASCII characters."
            );

        var hasFlowId = flowId is not null;
        var hasFlowOrigin = flowOrigin is not null;
        if (validateFlow && hasFlowId != hasFlowOrigin)
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope flow id and origin must be present together."
            );

        if (validateFlow)
        {
            if (flowOrigin is { } providedOrigin && !IsValidFlowOrigin(providedOrigin))
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    "ZLink envelope flow origin is invalid."
                );
            if (hasFlowId && !ZlinkStreamFlowId.IsValid(flowId))
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    "ZLink envelope flow id must be UUIDv7."
                );
        }
    }

    private static void ValidateHeaderString(
        ZLinkEnvelopeHeader header,
        string? value,
        bool validateUnicode
    )
    {
        if (value is null)
            return;
        if (validateUnicode)
        {
            try
            {
                _ = ZLinkMeshMetadataCodec.StrictUtf8.GetByteCount(value);
            }
            catch (EncoderFallbackException)
            {
                throw new ZLinkEnvelopeProtocolException(
                    header,
                    "ZLink envelope strings must contain valid Unicode."
                );
            }
        }
        if (value.Contains('\0'))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope strings must not contain NUL."
            );
    }

    private static ZLinkEnvelopeHeader InvalidProtocolHeader() =>
        new(
            ZLinkMessageKind.Command,
            string.Empty,
            string.Empty,
            DefaultContentType,
            null,
            null,
            null,
            null,
            null
        );

    public static (string? FlowId, ZLinkFlowOrigin? FlowOrigin) ValidFlow(
        ZLinkEnvelopeHeader header
    )
    {
        if (
            header.FlowId is null
            || header.FlowOrigin is not { } origin
            || !ZlinkStreamFlowId.IsValid(header.FlowId)
            || !IsValidFlowOrigin(origin)
        )
            return (null, null);

        return (header.FlowId, origin);
    }

    public static bool CanCorrelateReply(ZLinkEnvelopeHeader header)
    {
        var correlation = header.CorrelationId;
        if (string.IsNullOrEmpty(correlation) || correlation.Length > MaximumCorrelationIdBytes)
            return false;
        foreach (char value in correlation)
            if (!char.IsAscii(value))
                return false;
        return true;
    }

    public static string ProtocolErrorMessageName(ZLinkEnvelopeHeader header) =>
        string.IsNullOrWhiteSpace(header.MessageName)
            ? "$zlink.protocol-error"
            : header.MessageName;

    public static void ValidateDispatchHeader(ZLinkEnvelopeHeader header)
    {
        if (string.IsNullOrWhiteSpace(header.ChannelName))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope channel name is missing."
            );
        if (string.IsNullOrWhiteSpace(header.MessageName))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope message name is missing."
            );
        if (string.IsNullOrWhiteSpace(header.ContentType))
            throw new ZLinkEnvelopeProtocolException(
                header,
                "ZLink envelope content type is missing."
            );
    }
}
