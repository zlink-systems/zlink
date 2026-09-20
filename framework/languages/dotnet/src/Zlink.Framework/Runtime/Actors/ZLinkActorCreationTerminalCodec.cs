using System.Buffers.Binary;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkActorCreationTerminal(
    RequestResult Result,
    ServiceWireConstants.FrameworkErrorCode FailureCode,
    ZLinkActorCreationCompletion? Completion,
    IReadOnlyList<ReadOnlyMemory<byte>>? ReplyParts);

internal sealed record ZLinkActorCreationCompletion(
    ActorCreateResult Result,
    string ActorId,
    ulong ObjectGeneration);

internal static class ZLinkActorCreationTerminalCodec
{
    private const int MaximumBytes = 1024 * 1024;

    internal static byte[] Encode(
        ActorCreateOperationTerminal terminal,
        ZLinkCodecRegistryBuilder codecs)
    {
        var body = new MemoryStream();
        WriteU32(body, checked((uint)terminal.Result));
        WriteU32(body, checked((uint)terminal.FailureCode));
        body.WriteByte(terminal.Completion is null ? (byte)0 : (byte)1);
        if (terminal.Completion is { } completion)
            WriteCompletion(body, completion);

        byte[]? application = null;
        if (terminal.ReplyParts is { Count: > 0 } reply)
        {
            if (reply.Count < 2)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    "Actor creation reply envelope is incomplete.");
            var messages = reply.Select(static part => Message.From(part.Span)).ToArray();
            try
            {
                var header = ZLinkEnvelopeCodec.DecodeHeader(messages);
                application = ZLinkApplicationPayloadEnvelopeCodec.Encode(
                    string.IsNullOrEmpty(header.MessageName)
                        ? ZLinkApplicationPayloadEnvelopeCodec.CreationPacketName
                        : header.MessageName,
                    header.ContentType,
                    reply[1].Span);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(messages);
            }
        }
        body.WriteByte(application is null ? (byte)0 : (byte)1);
        if (application is not null)
            body.Write(application);

        var bytes = body.ToArray();
        var result = new byte[checked(5 + bytes.Length)];
        result[0] = 1;
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(1, 4), checked((uint)bytes.Length));
        bytes.CopyTo(result, 5);
        if (result.Length > MaximumBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                "Actor creation terminal exceeds 1 MiB.");
        return result;
    }

    internal static bool TryDecode(
        ReadOnlyMemory<byte> envelope,
        ZLinkCodecRegistryBuilder codecs,
        out ZLinkActorCreationTerminal terminal)
    {
        terminal = null!;
        var span = envelope.Span;
        if (span.Length is < 15 or > MaximumBytes
            || span[0] != 1
            || BinaryPrimitives.ReadUInt32BigEndian(span.Slice(1, 4)) != span.Length - 5)
            return false;
        var offset = 5;
        var result = (RequestResult)ReadU32(span, ref offset);
        var failure = (ServiceWireConstants.FrameworkErrorCode)ReadU32(span, ref offset);
        if (!TryBool(span, ref offset, out var hasCompletion))
            return false;
        ZLinkActorCreationCompletion? completion = null;
        if (hasCompletion && !TryReadCompletion(span, ref offset, out completion))
            return false;
        if (!TryBool(span, ref offset, out var hasPayload))
            return false;
        IReadOnlyList<ReadOnlyMemory<byte>>? reply = null;
        if (hasPayload)
        {
            var payload = envelope.Slice(offset);
            if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecode(payload, out var application))
                return false;
            using var raw = Message.From(application.Payload.Span);
            var message = ZLinkMessage.FromEnvelopePayload(
                application.ContentType,
                raw,
                codecs);
            var parts = ZLinkEnvelopeCodec.EncodeParts(
                new ZLinkEnvelopeHeader(
                    ZLinkMessageKind.Response,
                    application.PacketName,
                    string.Empty,
                    application.ContentType,
                    "0",
                    null, null, null, null),
                message,
                typeof(ZLinkMessage),
                codecs);
            try
            {
                reply = parts.Select(static part =>
                    (ReadOnlyMemory<byte>)part.AsReadOnlyMemory().ToArray()).ToArray();
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
            offset = span.Length;
        }
        if (offset != span.Length
            || result == RequestResult.Ok != (completion is not null)
            || !ServiceWireConstants.ValidTerminalFailure(
                (uint)result,
                (uint)failure)
            || result != RequestResult.Ok && hasPayload
            || completion?.Result == ActorCreateResult.Existing && hasPayload)
            return false;
        terminal = new ZLinkActorCreationTerminal(
            result,
            failure,
            completion,
            reply);
        return true;
    }

    private static void WriteCompletion(Stream stream, ActorCreateCompletion completion)
    {
        stream.WriteByte((byte)completion.Result);
        using var selected = new MemoryStream();
        if (completion.Result != ActorCreateResult.Rejected)
        {
            WriteText8(selected, completion.Actor.ActorId);
            WriteU64(selected, completion.Actor.ObjectGeneration);
        }
        WriteU16(stream, checked((ushort)selected.Length));
        selected.Position = 0;
        selected.CopyTo(stream);
    }

    private static bool TryReadCompletion(
        ReadOnlySpan<byte> span,
        ref int offset,
        out ZLinkActorCreationCompletion? completion)
    {
        completion = null;
        if (offset + 3 > span.Length) return false;
        var result = (ActorCreateResult)span[offset++];
        var length = ReadU16(span, ref offset);
        if (offset + length > span.Length
            || result is < ActorCreateResult.Existing or > ActorCreateResult.Rejected)
            return false;
        var end = offset + length;
        var actorId = string.Empty;
        ulong objectGeneration = 0;
        if (result != ActorCreateResult.Rejected)
        {
            if (!TryText8(span, ref offset, end, out actorId)
                || offset + 8 > end)
                return false;
            objectGeneration = ReadU64(span, ref offset);
            if (objectGeneration == 0 || offset != end) return false;
        }
        else if (length != 0) return false;
        offset = end;
        completion = new ZLinkActorCreationCompletion(
            result,
            actorId,
            objectGeneration);
        return true;
    }

    private static void WriteText8(Stream stream, string value)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(value);
        if (bytes.Length is 0 or > byte.MaxValue) throw new ArgumentOutOfRangeException(nameof(value));
        stream.WriteByte((byte)bytes.Length);
        stream.Write(bytes);
    }

    private static bool TryText8(
        ReadOnlySpan<byte> span,
        ref int offset,
        int end,
        out string value)
    {
        value = string.Empty;
        if (offset >= end) return false;
        var length = span[offset++];
        if (length == 0 || offset + length > end) return false;
        value = System.Text.Encoding.UTF8.GetString(span.Slice(offset, length));
        offset += length;
        return true;
    }

    private static bool TryBool(ReadOnlySpan<byte> span, ref int offset, out bool value)
    {
        value = false;
        if (offset >= span.Length || span[offset] > 1) return false;
        value = span[offset++] == 1;
        return true;
    }

    private static ushort ReadU16(ReadOnlySpan<byte> span, ref int offset)
    {
        var value = BinaryPrimitives.ReadUInt16BigEndian(span.Slice(offset, 2));
        offset += 2;
        return value;
    }

    private static uint ReadU32(ReadOnlySpan<byte> span, ref int offset)
    {
        var value = BinaryPrimitives.ReadUInt32BigEndian(span.Slice(offset, 4));
        offset += 4;
        return value;
    }

    private static ulong ReadU64(ReadOnlySpan<byte> span, ref int offset)
    {
        var value = BinaryPrimitives.ReadUInt64BigEndian(span.Slice(offset, 8));
        offset += 8;
        return value;
    }

    private static void WriteU16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteU32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteU64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }
}
