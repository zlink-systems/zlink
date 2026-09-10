using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Text;
using Google.Protobuf;
using BytesValue = Google.Protobuf.WellKnownTypes.BytesValue;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class MeshReplyPayloadOwnershipTests
{
    [Fact]
    public void Json_reply_keeps_native_frames_until_typed_decode_finishes()
    {
        var expected = new Probe(new string('x', 4096));
        var wire = Wire(expected, typeof(Probe));
        using var reply = Unpack(wire);
        AssertAlive(wire);
        Assert.True(reply.ApplicationPayloadView!.GetSpan(1).Overlaps(wire[1].AsReadOnlySpan()));
        Assert.Equal(expected, Decode<Probe>(reply));
        AssertDisposed(wire);
    }

    [Fact]
    public void Protobuf_reply_decodes_from_view_and_owns_its_result()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);
        var expected = new BytesValue { Value = ByteString.CopyFrom(new byte[262144]) };
        var wire = Wire(expected, typeof(BytesValue), codecs);
        using var reply = Unpack(wire);
        AssertAlive(wire);
        Assert.NotNull(reply.ApplicationPayloadView);
        Assert.True(reply.ApplicationPayloadView.GetSpan(1).Overlaps(wire[1].AsReadOnlySpan()));
        Assert.Equal(expected, Decode<BytesValue>(reply, codecs));
        AssertDisposed(wire);
    }

    [Fact]
    public void Memory_only_serializer_retains_safe_memory_after_native_disposal()
    {
        var serializer = new MemorySerializer();
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/x-owned-reply", serializer);
        var wire = Wire(new Probe("memory"), typeof(Probe), codecs);
        using var reply = Unpack(wire);
        AssertAlive(wire);
        Assert.Equal(new Probe("memory"), Decode<Probe>(reply, codecs));
        AssertDisposed(wire);
        Assert.Equal("memory", Encoding.UTF8.GetString(serializer.Received.Span));
    }

    [Fact]
    public void Returned_memory_and_framework_message_outlive_native_reply()
    {
        var expected = new Probe("retained");
        var memoryWire = Wire(expected, typeof(Probe));
        using var memoryReply = Unpack(memoryWire);
        var memory = Decode<ReadOnlyMemory<byte>>(memoryReply);
        AssertDisposed(memoryWire);
        Assert.Contains("retained", Encoding.UTF8.GetString(memory.Span));

        var messageWire = Wire(expected, typeof(Probe));
        using var messageReply = Unpack(messageWire);
        var message = Decode<ZLinkMessage>(messageReply);
        AssertDisposed(messageWire);
        Assert.Equal(expected, message.Decode<Probe>());
    }

    [Fact]
    public void Returned_native_message_is_independent_of_the_reply_frame()
    {
        var wire = Wire(new Probe("native"), typeof(Probe));
        using var reply = Unpack(wire);
        using var body = Decode<Message>(reply);
        AssertDisposed(wire);
        Assert.Contains("native", body.GetString());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Envelope_failure_disposes_owned_native_frames(bool remoteError)
    {
        using var header = remoteError
            ? ZLinkEnvelopeCodec.EncodeHeader(Header() with
            {
                Kind = ZLinkMessageKind.Error,
                ErrorCode = "invalid_operation",
                ErrorMessage = "remote failure"
            })
            : Message.From("malformed envelope");
        using var body = Message.From("null");
        var wire = Wrap([header, body]);
        using var reply = Unpack(wire);
        AssertAlive(wire);
        Assert.Throws<ZLinkFrameworkException>(() => Decode<Probe>(reply));
        AssertDisposed(wire);
    }

    [Fact]
    public void Cancellation_during_decode_disposes_owned_native_frames()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/x-owned-reply", new MemorySerializer(cancel: true));
        var wire = Wire(new Probe("cancel"), typeof(Probe), codecs);
        using var reply = Unpack(wire);
        var failure = Assert.Throws<ZLinkFrameworkException>(() => Decode<Probe>(reply, codecs));
        Assert.IsType<OperationCanceledException>(failure.InnerException);
        AssertDisposed(wire);
    }

    [Fact]
    public void Invalid_service_reply_disposes_native_frames_before_throwing()
    {
        var wire = new[] { Message.From("invalid service header"), Message.From("payload") };
        Assert.Throws<ZlinkRequestException>(() => Unpack(wire));
        AssertDisposed(wire);
    }

    private static ZLinkBackendRouteReceived Unpack(Message[] wire)
    {
        var result = Invoke(typeof(ZLinkManagedMeshNode).GetMethod(
            "DecodeDirectApplicationReply", BindingFlags.NonPublic | BindingFlags.Static)!,
            7UL, wire);
        try
        {
            // This also rejects an eager unpack that already closed its native
            // input before handing the reply to the typed decoder.
            AssertAlive(wire);
            return Assert.IsType<ZLinkBackendRouteReceived>(result);
        }
        catch
        {
            if (result is IDisposable owner) owner.Dispose();
            else if (result is IReadOnlyList<Message> parts) ZLinkMessageParts.DisposeAll(parts);
            throw;
        }
    }

    private static T Decode<T>(ZLinkBackendRouteReceived reply, ZLinkCodecRegistryBuilder? codecs = null)
    {
        var method = typeof(ZLinkClientCallCodec).GetMethods(BindingFlags.Public | BindingFlags.Static)
            .Single(method => method.Name == "DecodeEnvelopeReplyAndDispose"
                && method.GetParameters()[0].ParameterType == typeof(ZLinkBackendRouteReceived));
        return (T)Invoke(method.MakeGenericMethod(typeof(T)), reply,
            "empty", "failed", codecs, true)!;
    }

    private static object? Invoke(MethodInfo method, params object?[] arguments)
    {
        try { return method.Invoke(null, arguments); }
        catch (TargetInvocationException error) when (error.InnerException is not null)
        {
            ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
    }

    private static Message[] Wire(object value, Type type, ZLinkCodecRegistryBuilder? codecs = null)
    {
        var parts = ZLinkEnvelopeCodec.EncodeParts(Header(), value, type, codecs);
        try { return Wrap(parts); }
        finally { ZLinkMessageParts.DisposeAll(parts); }
    }

    private static Message[] Wrap(IReadOnlyList<Message> parts) =>
        [Message.From(ZLinkServiceWireCodec.EncodeReply(7, (int)RequestResult.Ok, 0)),
         ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts)];

    private static ZLinkEnvelopeHeader Header() =>
        new(ZLinkMessageKind.Response, "owned", nameof(Probe), "application/json", "reply-7",
            null, null, null, null);

    private static void AssertAlive(IEnumerable<Message> parts)
    {
        foreach (var part in parts) Assert.True(part.Size > 0);
    }

    private static void AssertDisposed(IEnumerable<Message> parts)
    {
        foreach (var part in parts) Assert.Throws<ObjectDisposedException>(() => part.Size);
    }

    public sealed record Probe(string Value);

    private sealed class MemorySerializer(bool cancel = false) : IZLinkMessageSerializer
    {
        internal ReadOnlyMemory<byte> Received { get; private set; }
        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From(Encoding.UTF8.GetBytes(((Probe)value).Value));
        public object Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            if (cancel) throw new OperationCanceledException();
            Received = payload.Bytes;
            return new Probe(Encoding.UTF8.GetString(payload.Bytes.Span));
        }
    }
}
