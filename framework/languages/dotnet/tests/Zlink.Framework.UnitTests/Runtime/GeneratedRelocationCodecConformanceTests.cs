using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class GeneratedRelocationCodecConformanceTests
{
    [Fact]
    public void RelocationEnvelope_RuntimeAndGeneratedCodecsMatchGoldenBytes()
    {
        var golden = ReadGolden("relocation-envelope-v1", "logicalHex");
        Assert.Equal(
            golden,
            ReadProjection("relocation-envelope-v1", "hex"));

        var runtime = ZLinkRelocationEnvelopeCodec.Decode(golden);
        var generated = ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(golden);

        Assert.Equal(golden, ZLinkRelocationEnvelopeCodec.Encode(runtime));
        Assert.Equal(
            golden,
            ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(generated));
    }

    [Fact]
    public void RelocationEnvelope_BothCodecsRejectAdjudicatedCrossSectionMutations()
    {
        var membershipMutation = ReadGolden(
            "relocation-envelope-v1", "logicalHex");
        membershipMutation[132] = 4;
        AssertRejectedByBoth(membershipMutation);

        var timerReferenceMutation = ReadGolden(
            "relocation-envelope-v1", "logicalHex");
        timerReferenceMutation[402] = (byte)'x';
        AssertRejectedByBoth(timerReferenceMutation);

        var golden = ReadGolden("relocation-envelope-v1", "logicalHex");
        var emptyApplicationStates = new byte[64];
        golden.AsSpan(0, 48).CopyTo(emptyApplicationStates);
        AssertRejectedByBoth(emptyApplicationStates);
    }

    [Fact]
    public async Task DurableTree_RuntimeAndGeneratedCodecsMatchGoldenBytes()
    {
        var chunkGolden = ReadGolden(
            "relocation-data-chunk-v1", "encodedHex");
        var manifestGolden = ReadGolden(
            "relocation-manifest-v1", "encodedHex");
        Assert.Equal(
            chunkGolden,
            ReadProjection("relocation-data-chunk-v1", "encodedHex"));
        Assert.Equal(
            manifestGolden,
            ReadProjection("relocation-manifest-v1", "encodedHex"));

        var generatedChunk =
            ServiceWirePilotCodec.DecodeRelocationDataChunkV1(chunkGolden);
        var generatedManifest =
            ServiceWirePilotCodec.DecodeRelocationManifestV1(manifestGolden);
        Assert.Equal(
            chunkGolden,
            ServiceWirePilotCodec.EncodeRelocationDataChunkV1(generatedChunk));
        Assert.Equal(
            manifestGolden,
            ServiceWirePilotCodec.EncodeRelocationManifestV1(generatedManifest));

        var reader = new GoldenTreeStore();
        reader.Seed("chunk-0", chunkGolden);
        reader.Seed("manifest", manifestGolden);
        var decoded = await ZLinkRelocationTreeStore.ReadAsync(
            reader,
            "manifest",
            ZLinkCrc32C.Compute(manifestGolden),
            CancellationToken.None);

        var writer = new GoldenTreeStore();
        var singleComponent = decoded.Envelope with
        {
            Participants = [decoded.Envelope.Participants[0]]
        };
        _ = await ZLinkRelocationTreeStore.PutAsync(
            writer,
            singleComponent,
            TimeSpan.FromHours(24),
            CancellationToken.None);

        Assert.Equal(chunkGolden, writer.Payloads["chunk-0"]);
        Assert.Equal(manifestGolden, writer.Payloads["manifest"]);
    }

    private static void AssertRejectedByBoth(byte[] encoded)
    {
        Assert.ThrowsAny<Exception>(
            () => ZLinkRelocationEnvelopeCodec.Decode(encoded));
        Assert.ThrowsAny<Exception>(
            () => ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(encoded));
    }

    private static byte[] ReadGolden(string name, string property) =>
        ReadFixture(
            $"framework/runtime/protocol/golden/{name}.json",
            property);

    private static byte[] ReadProjection(string name, string property) =>
        ReadFixture(
            $"framework/runtime/protocol/generated/fixtures/{name}-pilot.json",
            property);

    private static byte[] ReadFixture(string relativePath, string property)
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            relativePath);
        using var fixture = JsonDocument.Parse(File.ReadAllText(path));
        return Convert.FromHexString(
            fixture.RootElement.GetProperty(property).GetString()!);
    }

    private sealed class GoldenTreeStore : IZLinkRelocationRepository
    {
        internal Dictionary<string, byte[]> Payloads { get; } =
            new(StringComparer.Ordinal);

        internal void Seed(string reference, byte[] payload) =>
            Payloads.Add(reference, payload);

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            var reference = bytes.AsSpan(0, 4).SequenceEqual("ZLTC"u8)
                ? "chunk-0"
                : bytes.AsSpan(0, 4).SequenceEqual("ZLTM"u8)
                    ? "manifest"
                    : throw new InvalidDataException("Unexpected durable frame.");
            Payloads[reference] = bytes;
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult<ZLinkRelocationReadResult>(
                Payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());
        }

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();
    }
}
