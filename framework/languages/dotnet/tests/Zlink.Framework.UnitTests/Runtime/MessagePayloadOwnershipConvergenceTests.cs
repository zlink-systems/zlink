using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class MessagePayloadOwnershipConvergenceTests
{
    [Fact]
    public void ReadonlyAccessorAndDeserializeBudgetMatchSharedFixture()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "payload-ownership-v1.json")));
        var accessor = document.RootElement.GetProperty("accessorScenario");
        Assert.Equal(0, accessor.GetProperty("fullBufferCopies").GetInt32());

        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new CountingSerializer();
        codecs.AddSerializer("application/x-counting", serializer);
        using var nativePayload = Message.From("owned");
        var message = ZLinkMessage.FromEnvelopePayload(
            "application/x-counting",
            nativePayload,
            codecs);

        var views = Enumerable.Range(
                0,
                accessor.GetProperty("reads").GetInt32())
            .Select(_ => message.Decode<ReadOnlyMemory<byte>>())
            .ToArray();
        Assert.All(views, view => Assert.Equal("owned", Encoding.UTF8.GetString(view.Span)));
        Assert.True(MemoryMarshal.TryGetArray(
            views[0],
            out ArraySegment<byte> first));
        Assert.All(views.Skip(1), view =>
        {
            Assert.True(MemoryMarshal.TryGetArray(
                view,
                out ArraySegment<byte> next));
            Assert.Same(first.Array, next.Array);
            Assert.Equal(first.Offset, next.Offset);
            Assert.Equal(first.Count, next.Count);
        });

        var firstCopy = message.Decode<byte[]>();
        var secondCopy = message.Decode<byte[]>();
        Assert.Equal("owned", Encoding.UTF8.GetString(firstCopy));
        Assert.Equal("owned", Encoding.UTF8.GetString(secondCopy));
        Assert.NotSame(firstCopy, secondCopy);
        Assert.Equal(0, serializer.DeserializeCalls);

        Assert.Equal("owned", message.Decode<Probe>().Value);
        Assert.Equal("owned", message.Decode<Probe>().Value);
        Assert.Equal(
            document.RootElement
                .GetProperty("copyBudget")
                .GetProperty("maximumDeserializationsAfterAdmission")
                .GetInt32(),
            serializer.DeserializeCalls);
    }

    [Fact]
    public void AcceptedFrameAndAdmissionLeaseReleaseExactlyOnce()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "payload-ownership-v1.json")));
        var expectedReleases = document.RootElement
            .GetProperty("scenarios")
            .EnumerateArray()
            .Single(static scenario =>
                scenario.GetProperty("name").GetString()
                == "accepted-handler-success")
            .GetProperty("frameworkReleases")
            .GetInt32();
        var budget = new ZLinkInboundDispatchBudget(
            applicationHwmBytes: 1024);
        var lease = Assert.IsType<ZLinkInboundDispatchLease>(budget.Track(5));
        lease.StartDispatch();
        var actor = new ZLinkBackendActorRef(
            RoutingId.From("node"),
            "actor",
            1);
        var frame = new ZLinkSpotActorFrame(
            actor,
            actor,
            RoutingId.From("source"),
            RoutingId.From("session"),
            requestId: 0,
            flags: 0,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, 1),
                MessageFollowHopCount: 1,
                TargetNodeGeneration: 1,
                AuthorityOwnerGeneration: 1,
                OwnerLeaseGeneration: 1),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                RequestSeq: null,
                "ownership",
                ZlinkStreamMetadata.Empty),
            Message.From("owned"));
        var releases = 0;
        var batch = new ZLinkSpotActorFrameBatch(
            [frame],
            () => releases++,
            lease);

        batch.Dispose();
        batch.Dispose();

        Assert.Equal(expectedReleases, releases);
        Assert.Equal(0UL, budget.Snapshot().PendingPayloadBytes);
        Assert.Equal(0UL, budget.Snapshot().ActivePayloadBytes);
        Assert.Throws<ObjectDisposedException>(() => _ = frame.Body);
    }

    [Fact]
    public async Task ConcurrentTypedAccessDeserializesOnceForTheOwnerTurn()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new CountingSerializer();
        codecs.AddSerializer("application/x-counting", serializer);
        using var nativePayload = Message.From("owned");
        var message = ZLinkMessage.FromEnvelopePayload(
            "application/x-counting",
            nativePayload,
            codecs);
        using var start = new ManualResetEventSlim();
        var reads = Enumerable.Range(0, 64)
            .Select(_ => Task.Run(() =>
            {
                start.Wait();
                return message.Decode<Probe>();
            }))
            .ToArray();

        start.Set();
        var values = await Task.WhenAll(reads);

        Assert.Equal(1, serializer.DeserializeCalls);
        Assert.All(values, value => Assert.Same(values[0], value));
    }

    [Fact]
    public void FirstSuccessfulDecodeOwnsThePayloadAcrossTargetTypes()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new CountingSerializer();
        codecs.AddSerializer("application/x-counting", serializer);
        using var nativePayload = Message.From("owned");
        var message = ZLinkMessage.FromEnvelopePayload(
            "application/x-counting",
            nativePayload,
            codecs);

        var decoded = message.Decode<Probe>();

        Assert.Same(decoded, message.Decode<object>());
        Assert.Throws<InvalidCastException>(() => message.Decode<OtherProbe>());
        Assert.Equal(1, serializer.DeserializeCalls);
    }

    [Fact]
    public void RawStringDecodeAlsoRetainsTheFirstTypedOutcome()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var message = ZLinkMessage.FromStreamPayload(
            ZlinkStreamCodec.Raw,
            "owned"u8.ToArray(),
            codecs);

        var decoded = message.Decode<string>();

        Assert.Same(decoded, message.Decode<object>());
        Assert.Equal("owned", message.Decode<string>());
        Assert.Throws<InvalidCastException>(() => message.Decode<Probe>());
    }

    [Fact]
    public void EmptyTypedOutcomeCannotBecomeANonNullableValueLater()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var message = ZLinkMessage.FromEncoded(
            "application/json",
            ReadOnlyMemory<byte>.Empty,
            codecs);

        Assert.Null(message.Decode<string>());
        Assert.Throws<InvalidCastException>(() => message.Decode<int>());
    }

    [Fact]
    public async Task FailedCacheOwnerPublishesOneFailureWithoutConcurrentRedeserialize()
    {
        using var firstDecodeEntered = new ManualResetEventSlim();
        using var releaseFirstFailure = new ManualResetEventSlim();
        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new FailFirstSerializer(firstDecodeEntered, releaseFirstFailure);
        codecs.AddSerializer("application/x-fail-first", serializer);
        using var nativePayload = Message.From("owned");
        var message = ZLinkMessage.FromEnvelopePayload(
            "application/x-fail-first",
            nativePayload,
            codecs);

        var failedOwner = Task.Run(() => message.Decode<Probe>());
        Assert.True(firstDecodeEntered.Wait(TimeSpan.FromSeconds(5)));

        using var retriesReady = new CountdownEvent(8);
        var concurrentReads = Enumerable.Range(0, retriesReady.InitialCount)
            .Select(index => Task.Run(() =>
            {
                retriesReady.Signal();
                return index % 2 == 0
                    ? Record.Exception(() => message.Decode<Probe>())
                    : Record.Exception(() => message.Decode<OtherProbe>());
            }))
            .ToArray();
        Assert.True(retriesReady.Wait(TimeSpan.FromSeconds(5)));
        releaseFirstFailure.Set();

        var ownerFailure = await Assert.ThrowsAsync<InvalidOperationException>(
            () => failedOwner);
        var concurrentFailures = await Task.WhenAll(concurrentReads);
        var repeatedFailure = Record.Exception(() => message.Decode<OtherProbe>());

        Assert.Equal(1, serializer.DeserializeCalls);
        Assert.Equal("The first decode attempt fails.", ownerFailure.Message);
        Assert.All(concurrentFailures, AssertDecodeFailure);
        AssertDecodeFailure(repeatedFailure);

        static void AssertDecodeFailure(Exception? exception)
        {
            var failure = Assert.IsType<InvalidOperationException>(exception);
            Assert.Equal("The first decode attempt fails.", failure.Message);
        }
    }

    [Fact]
    public void WrongSerializerTypePublishesOneFailureToEveryConcurrentTarget()
    {
        const int readerCount = 32;
        using var deserializeEntered = new ManualResetEventSlim();
        using var releaseDeserialize = new ManualResetEventSlim();
        var codecs = new ZLinkCodecRegistryBuilder();
        var serializer = new WrongTypeSerializer(
            deserializeEntered,
            releaseDeserialize);
        codecs.AddSerializer("application/x-wrong-type", serializer);
        using var nativePayload = Message.From("owned");
        var message = ZLinkMessage.FromEnvelopePayload(
            "application/x-wrong-type",
            nativePayload,
            codecs);
        Exception? ownerFailure = null;
        var owner = new Thread(() =>
            ownerFailure = Record.Exception(() => message.Decode<Probe>()));
        owner.Start();
        Assert.True(deserializeEntered.Wait(TimeSpan.FromSeconds(5)));

        using var readersReady = new CountdownEvent(readerCount);
        using var startReaders = new ManualResetEventSlim();
        var readerFailures = new Exception?[readerCount];
        var readers = Enumerable.Range(0, readerCount)
            .Select(index => new Thread(() =>
            {
                readersReady.Signal();
                startReaders.Wait();
                readerFailures[index] = Record.Exception(
                    () => message.Decode<object>());
            }))
            .ToArray();
        foreach (var reader in readers)
            reader.Start();
        Assert.True(readersReady.Wait(TimeSpan.FromSeconds(5)));
        startReaders.Set();

        // Give every reader a chance to observe the in-progress owner state
        // before the serializer returns its invalid runtime type.
        Thread.Sleep(20);
        releaseDeserialize.Set();

        Assert.True(owner.Join(TimeSpan.FromSeconds(5)));
        foreach (var reader in readers)
            Assert.True(reader.Join(TimeSpan.FromSeconds(5)));

        var retainedFailure = Assert.IsType<InvalidCastException>(ownerFailure);
        Assert.All(readerFailures, failure =>
            Assert.Same(retainedFailure, Assert.IsType<InvalidCastException>(failure)));
        Assert.Equal(1, serializer.DeserializeCalls);
    }

    private sealed record Probe(string Value);

    private sealed record OtherProbe(string Value);

    private sealed class CountingSerializer : IZLinkMessageSerializer
    {
        internal int DeserializeCalls { get; private set; }

        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From(
                Encoding.UTF8.GetBytes(((Probe)value).Value));

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            DeserializeCalls++;
            return new Probe(Encoding.UTF8.GetString(payload.Bytes.Span));
        }
    }

    private sealed class FailFirstSerializer(
        ManualResetEventSlim firstDecodeEntered,
        ManualResetEventSlim releaseFirstFailure) : IZLinkMessageSerializer
    {
        private int _deserializeCalls;

        internal int DeserializeCalls => Volatile.Read(ref _deserializeCalls);

        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From(
                Encoding.UTF8.GetBytes(((Probe)value).Value));

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            var call = Interlocked.Increment(ref _deserializeCalls);
            if (call == 1)
            {
                firstDecodeEntered.Set();
                if (!releaseFirstFailure.Wait(TimeSpan.FromSeconds(5)))
                    throw new TimeoutException("Timed out waiting to release the first decode failure.");
                throw new InvalidOperationException("The first decode attempt fails.");
            }

            return new Probe(Encoding.UTF8.GetString(payload.Bytes.Span));
        }
    }

    private sealed class WrongTypeSerializer(
        ManualResetEventSlim deserializeEntered,
        ManualResetEventSlim releaseDeserialize) : IZLinkMessageSerializer
    {
        private int _deserializeCalls;

        internal int DeserializeCalls => Volatile.Read(ref _deserializeCalls);

        public ZLinkEncodedPayload Serialize(object value, Type type) =>
            ZLinkEncodedPayload.From(
                Encoding.UTF8.GetBytes(((Probe)value).Value));

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            Interlocked.Increment(ref _deserializeCalls);
            deserializeEntered.Set();
            if (!releaseDeserialize.Wait(TimeSpan.FromSeconds(5)))
                throw new TimeoutException(
                    "Timed out waiting to return the invalid decoded type.");
            return new OtherProbe(Encoding.UTF8.GetString(payload.Bytes.Span));
        }
    }
}
