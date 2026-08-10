using System.Text;
using System.Text.Json;
using System.Reflection;
using System.Reflection.Emit;
using MessagePack;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Codecs;
using StringValue = Google.Protobuf.WellKnownTypes.StringValue;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CustomSerializerEnvelopeTests
{
    [Fact]
    public void CustomSerializer_Sets_ContentType_And_RoundTrips_Body()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());

        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "orders",
            nameof(Probe),
            ZLinkEnvelopeCodec.DefaultContentType,
            "request-1",
            null,
            null,
            null,
            null);

        var parts = ZLinkEnvelopeCodec.EncodeParts(header, new Probe("hello"), typeof(Probe), codecs);

        // The custom serializer's content type is carried on the envelope header.
        var decodedHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
        Assert.Equal("application/avro", decodedHeader.ContentType);

        // The body is encoded with the custom serializer, not JSON.
        Assert.Equal("AVRO:hello", parts[1].GetString());

        // And it decodes back through the same registered serializer.
        var decoded = ZLinkEnvelopeCodec.DecodeBody(parts, typeof(Probe), codecs);
        Assert.Equal(new Probe("hello"), decoded);
    }

    [Fact]
    public void CodecExtension_Can_Register_Custom_Serializer()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(new MarkerCodecExtension());

        var custom = codecs.SingleCustomSerializer();

        Assert.NotNull(custom);
        Assert.Equal("application/avro", custom.Value.ContentType);
        Assert.IsType<MarkerSerializer>(custom.Value.Serializer);
    }

    [Fact]
    public async Task Serializer_Type_Cache_Is_Safe_For_Concurrent_First_Use()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new MarkerSerializer();
        codecs.AddSerializer("application/avro", serializer);

        var results = await Task.WhenAll(Enumerable.Range(0, 256).Select(_ => Task.Run(() =>
        {
            Assert.True(codecs.TryResolveSerializer(typeof(Probe), out var contentType, out var resolved));
            return (contentType, resolved);
        })));

        Assert.All(results, result =>
        {
            Assert.Equal("application/avro", result.contentType);
            Assert.Same(serializer, result.resolved);
        });
    }

    [Fact]
    public void Protobuf_Extension_RoundTrips_Protobuf_Payload()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);
        var value = new StringValue { Value = "hello" };

        var parts = ZLinkEnvelopeCodec.EncodeParts(CreateHeader(nameof(StringValue)), value, typeof(StringValue),
            codecs);

        Assert.Equal("application/x-protobuf", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
        var decoded = Assert.IsType<StringValue>(ZLinkEnvelopeCodec.DecodeBody(parts, typeof(StringValue), codecs));
        Assert.Equal("hello", decoded.Value);
    }

    [Fact]
    public void MessagePack_Extension_RoundTrips_MessagePack_Payload()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkMessagePackCodec.Default);
        var value = new PackedProbe("hello");

        var parts = ZLinkEnvelopeCodec.EncodeParts(CreateHeader(nameof(PackedProbe)), value, typeof(PackedProbe),
            codecs);

        Assert.Equal("application/x-msgpack", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
        var decoded = Assert.IsType<PackedProbe>(ZLinkEnvelopeCodec.DecodeBody(parts, typeof(PackedProbe), codecs));
        Assert.Equal(value, decoded);
    }

    [Fact]
    public void SpotCreate_Json_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        AssertEnvelopeRequestAndReplyRoundTrip(
            new ZLinkCodecRegistryBuilder(),
            new Probe("create-request"),
            new Probe("create-reply"),
            "application/json");
    }

    [Fact]
    public void SpotCreate_Protobuf_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);

        AssertEnvelopeRequestAndReplyRoundTrip(
            codecs,
            new StringValue { Value = "create-request" },
            new StringValue { Value = "create-reply" },
            "application/x-protobuf");
    }

    [Fact]
    public void SpotCreate_MessagePack_Extension_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkMessagePackCodec.Default);
        var request = ZLinkMessage.From(new PackedProbe("create"));

        var encoded = request.Encode(codecs);
        using var payload = Message.From(encoded.Payload.Bytes.Span);
        var received = ZLinkMessage.FromEnvelopePayload(encoded.ContentType, payload, codecs);

        Assert.Equal("application/x-msgpack", received.ContentType);
        Assert.Equal(new PackedProbe("create"), received.Decode<PackedProbe>());
    }

    [Fact]
    public void SpotCreate_CustomSerializer_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());

        AssertEnvelopeRequestAndReplyRoundTrip(
            codecs,
            new Probe("create-request"),
            new Probe("create-reply"),
            "application/avro");
    }

    [Fact]
    public void Session_Json_RoundTrips_Through_Framework_Message()
    {
        AssertStreamRoundTrip(
            new ZLinkCodecRegistryBuilder(),
            new Probe("session"),
            ZlinkStreamCodec.Json,
            "application/json");
    }

    [Fact]
    public void Session_Protobuf_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);

        AssertStreamRoundTrip(
            codecs,
            new StringValue { Value = "session" },
            ZlinkStreamCodec.Protobuf,
            "application/x-protobuf");
    }

    [Fact]
    public void Session_MessagePack_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkMessagePackCodec.Default);

        AssertStreamRoundTrip(
            codecs,
            new PackedProbe("session"),
            ZlinkStreamCodec.MessagePack,
            "application/x-msgpack");
    }

    [Fact]
    public void Session_CustomSerializer_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(AvroStreamCodecExtension.Instance);

        AssertStreamRoundTrip(
            codecs,
            new Probe("session"),
            ZlinkStreamCodec.Protobuf,
            "application/avro");
    }

    [Fact]
    public void ActorJoin_Json_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        AssertEnvelopeRequestAndReplyRoundTrip(
            new ZLinkCodecRegistryBuilder(),
            new Probe("join-request"),
            new Probe("join-reply"),
            "application/json");
    }

    [Fact]
    public void ActorJoin_Protobuf_Extension_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);
        var request = ZLinkMessage.From(new StringValue { Value = "join" });

        var encoded = request.Encode(codecs);
        using var payload = Message.From(encoded.Payload.Bytes.Span);
        var received = ZLinkMessage.FromEnvelopePayload(encoded.ContentType, payload, codecs);

        Assert.Equal("application/x-protobuf", received.ContentType);
        Assert.Equal("join", received.Decode<StringValue>().Value);
    }

    [Fact]
    public void ActorJoin_MessagePack_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkMessagePackCodec.Default);

        AssertEnvelopeRequestAndReplyRoundTrip(
            codecs,
            new PackedProbe("join-request"),
            new PackedProbe("join-reply"),
            "application/x-msgpack");
    }

    [Fact]
    public void ActorJoin_CustomSerializer_RoundTrips_Reply_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());
        var reply = ZLinkMessage.From(new Probe("accepted"));

        var encoded = reply.Encode(codecs);
        using var payload = Message.From(encoded.Payload.Bytes.Span);
        var received = ZLinkMessage.FromEnvelopePayload(encoded.ContentType, payload, codecs);

        Assert.Equal("application/avro", received.ContentType);
        Assert.Equal(new Probe("accepted"), received.Decode<Probe>());
    }

    [Fact]
    public void ActorJoin_CustomSerializer_RoundTrips_Request_And_Reply_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());

        AssertEnvelopeRequestAndReplyRoundTrip(
            codecs,
            new Probe("join-request"),
            new Probe("join-reply"),
            "application/avro");
    }

    [Fact]
    public void Binary_Extensions_Fall_Back_To_Json_For_Unsupported_Payload()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(ZLinkProtobufCodec.Default);
        codecs.Use(ZLinkMessagePackCodec.Default);
        var value = new Probe("hello");

        var parts = ZLinkEnvelopeCodec.EncodeParts(CreateHeader(nameof(Probe)), value, typeof(Probe), codecs);

        Assert.Equal("application/json", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
        var decoded = Assert.IsType<Probe>(ZLinkEnvelopeCodec.DecodeBody(parts, typeof(Probe), codecs));
        Assert.Equal(value, decoded);
    }

    [Fact]
    public void Without_Custom_Serializer_Body_Stays_Json()
    {
        var codecs = new ZLinkCodecRegistryBuilder();

        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "orders",
            nameof(Probe),
            ZLinkEnvelopeCodec.DefaultContentType,
            "request-1",
            null,
            null,
            null,
            null);

        var parts = ZLinkEnvelopeCodec.EncodeParts(header, new Probe("hello"), typeof(Probe), codecs);

        Assert.Equal("application/json", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
        var decoded = ZLinkEnvelopeCodec.DecodeBody(parts, typeof(Probe), codecs);
        Assert.Equal(new Probe("hello"), decoded);
    }

    [Fact]
    public void EncodeParts_Resolves_Unsupported_Serializer_Type_Once()
    {
        var resolutionCalls = 0;
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/avro",
            new MarkerSerializer(),
            _ =>
            {
                resolutionCalls++;
                return false;
            });

        var parts = ZLinkEnvelopeCodec.EncodeParts(
            CreateHeader(nameof(Probe)),
            new Probe("hello"),
            typeof(Probe),
            codecs);
        try
        {
            Assert.Equal(1, resolutionCalls);
            Assert.Equal("application/json", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);

            ZLinkMessageParts.DisposeAll(parts);
            parts = ZLinkEnvelopeCodec.EncodeParts(
                CreateHeader(nameof(Probe)),
                new Probe("again"),
                typeof(Probe),
                codecs);
            Assert.Equal(1, resolutionCalls);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public async Task Serializer_Type_First_Use_Is_Single_Flight()
    {
        var resolutionCalls = 0;
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/avro",
            new MarkerSerializer(),
            _ =>
            {
                Interlocked.Increment(ref resolutionCalls);
                Thread.Sleep(5);
                return true;
            });
        using var start = new ManualResetEventSlim();

        var resolutions = Enumerable.Range(0, 32)
            .Select(_ => Task.Run(() =>
            {
                start.Wait();
                return codecs.TryResolveSerializer(
                    typeof(Probe),
                    out var contentType,
                    out var serializer)
                    && contentType == "application/avro"
                    && serializer is MarkerSerializer;
            }))
            .ToArray();
        start.Set();

        Assert.All(await Task.WhenAll(resolutions), Assert.True);
        Assert.Equal(1, resolutionCalls);
    }

    [Fact]
    public void Serializer_Type_Cache_Does_Not_Evict_Existing_Entries_After_Saturation()
    {
        var resolutions = new Dictionary<Type, int>();
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/avro",
            new MarkerSerializer(),
            type =>
            {
                resolutions[type] = resolutions.GetValueOrDefault(type) + 1;
                return type == typeof(Probe);
            });

        Assert.True(codecs.TryResolveSerializer(
            typeof(Probe),
            out _,
            out _));

        var assembly = AssemblyBuilder.DefineDynamicAssembly(
            new AssemblyName("Zlink.SerializerCacheSaturation"),
            AssemblyBuilderAccess.Run);
        var module = assembly.DefineDynamicModule("Main");
        Type? uncachedType = null;
        for (var index = 0; index < 1_024; index++)
        {
            var type = module.DefineType($"Payload{index}").CreateTypeInfo()!.AsType();
            Assert.False(codecs.TryResolveSerializer(type, out _, out _));
            uncachedType = type;
        }

        Assert.True(codecs.TryResolveSerializer(
            typeof(Probe),
            out _,
            out _));
        Assert.Equal(1, resolutions[typeof(Probe)]);
        Assert.NotNull(uncachedType);
        Assert.False(codecs.TryResolveSerializer(uncachedType, out _, out _));
        Assert.Equal(2, resolutions[uncachedType]);
    }

    [Fact]
    public void ZLinkMessage_Resolves_Serializer_Type_Once()
    {
        var resolutionCalls = 0;
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/avro",
            new MarkerSerializer(),
            _ =>
            {
                resolutionCalls++;
                return true;
            });

        var encoded = ZLinkMessage.From(new Probe("hello")).Encode(codecs);

        Assert.Equal(1, resolutionCalls);
        Assert.Equal("application/avro", encoded.ContentType);
        Assert.Equal("AVRO:hello", Encoding.UTF8.GetString(encoded.Payload.Bytes.Span));
    }

    [Fact]
    public void StreamPayload_CustomSerializer_RoundTrips_Through_Framework_Message()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.Use(AvroStreamCodecExtension.Instance);

        var encoded = ZLinkStreamPacketPayloadCodec.Encode(new Probe("hello"), typeof(Probe), codecs);

        Assert.Equal(ZlinkStreamCodec.Protobuf, encoded.Codec);
        Assert.Equal("AVRO:hello", Encoding.UTF8.GetString(encoded.Payload.Span));

        using var payload = Message.From(encoded.Payload.Span);
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            encoded.Codec,
            ZlinkStreamHeaderFlags.None,
            null,
            "orders.created",
            ZlinkStreamMetadata.Empty);

        var message = ZLinkStreamPacketPayloadCodec.DecodeMessage(
            header,
            payload,
            codecs,
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());

        Assert.Equal("application/avro", message.ContentType);
        Assert.Equal(ZlinkStreamCodec.Protobuf, message.StreamCodec);
        Assert.Equal(new Probe("hello"), message.Decode<Probe>());
    }

    [Fact]
    public void StreamPayload_Missing_Codec_Extension_Fails_Decode()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Protobuf,
            ZlinkStreamHeaderFlags.None,
            null,
            "orders.created",
            ZlinkStreamMetadata.Empty);

        using var payload = Message.From("AVRO:hello");
        var message = ZLinkStreamPacketPayloadCodec.DecodeMessage(
            header,
            payload,
            codecs,
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());

        var error = Assert.Throws<InvalidOperationException>(() => message.Decode<Probe>());
        Assert.Contains("no matching codec extension is registered", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Later_Fallback_Serializer_Wins()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());
        var selected = new ReplacementSerializer();
        codecs.AddSerializer("application/thrift", selected);

        var fallback = codecs.SingleCustomSerializer();
        Assert.NotNull(fallback);
        Assert.Equal("application/thrift", fallback.Value.ContentType);
        Assert.Same(selected, fallback.Value.Serializer);
    }

    [Fact]
    public void Codec_Content_Type_And_Receive_Lookup_Match_Shared_Fixture()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "codec-selection-v1.json")));
        var root = document.RootElement;
        Assert.Equal("zlink.framework.codec-selection", root.GetProperty("fixture").GetString());
        Assert.Equal(1, root.GetProperty("version").GetInt32());

        foreach (var scenario in root.GetProperty("normalizationScenarios").EnumerateArray())
        {
            var codecs = new ZLinkCodecRegistryBuilder();
            var input = scenario.GetProperty("input").GetString()!;
            if (scenario.TryGetProperty("expectedError", out _))
            {
                Assert.Throws<ArgumentException>(() =>
                    codecs.AddSerializer(input, new MarkerSerializer()));
                continue;
            }

            codecs.AddSerializer(input, new MarkerSerializer());
            Assert.Equal(
                scenario.GetProperty("expected").GetString(),
                Assert.Single(codecs.Serializers).Key);
        }

        var duplicate = root.GetProperty("normalizedDuplicateScenario");
        var registrations = duplicate.GetProperty("registrationInputs");
        var replacement = new ReplacementSerializer();
        var duplicateCodecs = new ZLinkCodecRegistryBuilder();
        duplicateCodecs.AddSerializer(registrations[0].GetString()!, new MarkerSerializer());
        duplicateCodecs.AddSerializer(registrations[1].GetString()!, replacement);
        Assert.Equal(
            duplicate.GetProperty("finalEntryCount").GetInt32(),
            duplicateCodecs.Serializers.Count);
        Assert.Same(replacement, duplicateCodecs.Serializers["application/x-base"]);

        var receiveCodecs = new ZLinkCodecRegistryBuilder();
        receiveCodecs.AddSerializer("application/x-base", new MarkerSerializer());
        foreach (var scenario in root.GetProperty("receiveScenarios").EnumerateArray())
        {
            var contentType = scenario.GetProperty("wireContentType").GetString()!;
            var expectedSuccess = scenario.GetProperty("expectedTerminal").GetString()
                                  == "success";
            Assert.Equal(
                expectedSuccess,
                receiveCodecs.TryGetSerializer(contentType, out _));
        }
    }

    [Fact]
    public void Later_Matching_Declared_Type_Selector_Wins()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer(
            "application/x-first",
            new MarkerSerializer(),
            static type => type == typeof(Probe));
        var selected = new ReplacementSerializer();
        codecs.AddSerializer(
            "application/x-second",
            selected,
            static type => type == typeof(Probe));

        Assert.True(codecs.TryResolveSerializer(
            typeof(Probe),
            out var contentType,
            out var serializer));
        Assert.Equal("application/x-second", contentType);
        Assert.Same(selected, serializer);
    }

    [Fact]
    public void Codec_Registry_Rejects_Registration_After_Runtime_Startup_Freeze()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/x-before", new MarkerSerializer());

        codecs.Freeze();

        Assert.Throws<InvalidOperationException>(() =>
            codecs.AddSerializer("application/x-after", new ReplacementSerializer()));
        Assert.Throws<InvalidOperationException>(() =>
            codecs.RegisterStreamCodec("application/x-stream", ZlinkStreamCodec.MessagePack));
        Assert.Single(codecs.Serializers);
    }

    [Fact]
    public void Received_Message_Keeps_The_Codec_Snapshot_From_Its_Creation()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/avro", new MarkerSerializer());
        using var payload = Message.From("AVRO:original");
        var received = ZLinkMessage.FromEnvelopePayload("application/avro", payload, codecs);

        codecs.AddSerializer("application/avro", new ReplacementSerializer());
        using var laterPayload = Message.From("AVRO:later");
        var later = ZLinkMessage.FromEnvelopePayload("application/avro", laterPayload, codecs);

        Assert.Equal(new Probe("original"), received.Decode<Probe>());
        Assert.Equal(new Probe("replacement"), later.Decode<Probe>());
    }

    private static ZLinkEnvelopeHeader CreateHeader(string messageName)
    {
        return new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "orders",
            messageName,
            ZLinkEnvelopeCodec.DefaultContentType,
            "request-1",
            null,
            null,
            null,
            null);
    }

    private static void AssertEnvelopeRequestAndReplyRoundTrip<TRequest, TReply>(
        ZLinkCodecRegistryBuilder codecs,
        TRequest request,
        TReply reply,
        string expectedContentType)
    {
        var receivedRequest = EnvelopeRoundTrip(codecs, request, expectedContentType);
        AssertDecodedEquals(request, receivedRequest.Decode<TRequest>());

        var receivedReply = EnvelopeRoundTrip(codecs, reply, expectedContentType);
        AssertDecodedEquals(reply, receivedReply.Decode<TReply>());
    }

    private static ZLinkMessage EnvelopeRoundTrip<T>(
        ZLinkCodecRegistryBuilder codecs,
        T value,
        string expectedContentType)
    {
        var encoded = ZLinkMessage.From(value).Encode(codecs);
        using var payload = Message.From(encoded.Payload.Bytes.Span);
        var received = ZLinkMessage.FromEnvelopePayload(encoded.ContentType, payload, codecs);
        Assert.Equal(expectedContentType, received.ContentType);
        return received;
    }

    private static void AssertStreamRoundTrip<T>(
        ZLinkCodecRegistryBuilder codecs,
        T value,
        ZlinkStreamCodec expectedCodec,
        string expectedContentType)
    {
        var encoded = ZLinkStreamPacketPayloadCodec.Encode(value, typeof(T), codecs);

        Assert.Equal(expectedCodec, encoded.Codec);

        using var payload = Message.From(encoded.Payload.Span);
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            encoded.Codec,
            ZlinkStreamHeaderFlags.None,
            null,
            "orders.created",
            ZlinkStreamMetadata.Empty);

        var message = ZLinkStreamPacketPayloadCodec.DecodeMessage(
            header,
            payload,
            codecs,
            ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());

        Assert.Equal(expectedContentType, message.ContentType);
        Assert.Equal(expectedCodec, message.StreamCodec);
        AssertDecodedEquals(value, message.Decode<T>());
    }

    private static void AssertDecodedEquals<T>(T expected, T actual)
    {
        if (expected is StringValue expectedString && actual is StringValue actualString)
        {
            Assert.Equal(expectedString.Value, actualString.Value);
            return;
        }

        Assert.Equal(expected, actual);
    }

    private sealed record Probe(string Text);

    [MessagePackObject]
    public sealed record PackedProbe([property: Key(0)] string Text);

    private sealed class MarkerSerializer : IZLinkMessageSerializer
    {
        public ZLinkEncodedPayload Serialize(object value, Type type)
        {
            var probe = (Probe)value;
            return ZLinkEncodedPayload.From(Encoding.UTF8.GetBytes("AVRO:" + probe.Text));
        }

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            var text = Encoding.UTF8.GetString(payload.Bytes.Span);
            var value = text.StartsWith("AVRO:", StringComparison.Ordinal) ? text["AVRO:".Length..] : text;
            return new Probe(value);
        }
    }

    private sealed class ReplacementSerializer : IZLinkMessageSerializer
    {
        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From("replacement"u8.ToArray());

        public object? Deserialize(ZLinkEncodedPayload payload, Type type) =>
            new Probe("replacement");
    }

    private sealed class MarkerCodecExtension : IZLinkCodecExtension
    {
        public void Register(IZLinkCodecRegistrar codecs)
        {
            codecs.AddSerializer("application/avro", new MarkerSerializer());
        }
    }

    private sealed class AvroStreamCodecExtension :
        IZLinkCodecExtension,
        IZlinkStreamCodecRegistration
    {
        public static AvroStreamCodecExtension Instance { get; } = new();

        public string ContentType => "application/avro";

        public ZlinkStreamCodec Codec => ZlinkStreamCodec.Protobuf;

        public void Register(IZLinkCodecRegistrar codecs)
        {
            codecs.AddSerializer("application/avro", new MarkerSerializer());
        }
    }
}
