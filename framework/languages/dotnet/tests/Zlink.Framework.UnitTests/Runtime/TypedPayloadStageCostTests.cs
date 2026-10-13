using Google.Protobuf;
using Google.Protobuf.WellKnownTypes;
using Xunit.Abstractions;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class TypedPayloadStageCostTests(ITestOutputHelper output)
{
    [Fact]
    public void Report_warm_typed_encode_and_decode_allocations()
    {
        const int payloadSize = 1024;
        var json = new Payload { Body = new byte[payloadSize] };
        var protobuf = new BytesValue { Value = ByteString.CopyFrom(json.Body) };
        var jsonCodecs = new ZLinkCodecRegistryBuilder();
        var protobufCodecs = new ZLinkCodecRegistryBuilder();
        protobufCodecs.Use(ZLinkProtobufCodec.Default);
        using var jsonPart = ZLinkEnvelopeCodec.EncodeBody(json, typeof(Payload), jsonCodecs);
        using var protobufPart = ZLinkEnvelopeCodec.EncodeBody(
            protobuf,
            typeof(BytesValue),
            protobufCodecs,
            out var protobufContentType
        );
        Measure(
            "json-encode",
            () =>
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeBody(
                    json,
                    typeof(Payload),
                    jsonCodecs
                );
            }
        );
        Measure(
            "json-decode",
            () =>
                ZLinkEnvelopeCodec.DecodeBody(
                    jsonPart,
                    typeof(Payload),
                    ZLinkEnvelopeCodec.DefaultContentType,
                    jsonCodecs
                )
        );
        Measure(
            "protobuf-encode",
            () =>
            {
                using var encoded = ZLinkEnvelopeCodec.EncodeBody(
                    protobuf,
                    typeof(BytesValue),
                    protobufCodecs
                );
            }
        );
        Measure(
            "protobuf-decode",
            () =>
                ZLinkEnvelopeCodec.DecodeBody(
                    protobufPart,
                    typeof(BytesValue),
                    protobufContentType,
                    protobufCodecs
                )
        );
        using var caller = new CancellationTokenSource();
        Measure(
            "backpressure-linked-caller-owner",
            () =>
            {
                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(caller.Token);
                using var completion = new ZLinkRequestCompletion<IReadOnlyList<Message>>(
                    deadline.Token
                );
                GC.KeepAlive(completion.Task);
            }
        );
        var decoded = Assert.IsType<Payload>(
            ZLinkEnvelopeCodec.DecodeBody(
                jsonPart,
                typeof(Payload),
                ZLinkEnvelopeCodec.DefaultContentType,
                jsonCodecs
            )
        );
        Assert.Equal(json.Body, decoded.Body);
    }

    private void Measure(string stage, Action operation)
    {
        const int iterations = 1024;
        const int warmupIterations = 256;
        for (var index = 0; index < warmupIterations; index++)
            operation();
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            operation();
        var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
        output.WriteLine(
            $"stage={stage} iterations={iterations} bytes={bytes} bytes/message={bytes / iterations}"
        );
    }

    public sealed class Payload
    {
        public byte[] Body { get; init; } = [];
    }
}
