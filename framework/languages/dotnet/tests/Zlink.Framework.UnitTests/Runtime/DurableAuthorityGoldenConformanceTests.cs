using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Cross-language golden conformance for the durable authority payload
/// wire format. framework/runtime/protocol/golden/durable-authority-v1.json
/// carries an instance-spot case with an activationRecoveryState -- .NET has
/// no production codec that materializes that union case (only
/// ZLinkUserSpotAuthorityPayloadCodec for user spots and
/// ZLinkActorAuthorityPayloadCodec for actors exist today), so this test
/// stays at the ZLAU envelope level: it verifies magic/version/flags/length/
/// CRC-32C and treats the body as an opaque byte-exact round trip, matching
/// the shared envelope shape those two production codecs already implement
/// (see ZLinkUserSpotAuthorityPayloadCodec.Encode/TryDecode).
/// </summary>
public sealed class DurableAuthorityGoldenConformanceTests
{
    [Fact]
    public void DurableAuthorityGolden_envelope_round_trips_byte_exactly()
    {
        var fixturePath = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/durable-authority-v1.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var root = fixture.RootElement;

        Assert.Equal("authority-payload-v1", root.GetProperty("format").GetString());
        var consumers = root.GetProperty("consumers").EnumerateArray()
            .Select(item => item.GetString())
            .ToArray();
        Assert.Contains("dotnet", consumers);

        var encodedHex = root.GetProperty("encodedHex").GetString()!;
        var encoded = Convert.FromHexString(encodedHex);

        Assert.True(DurableAuthorityEnvelope.TryDecode(encoded, out var envelope));
        Assert.Equal(
            encodedHex,
            Convert.ToHexString(DurableAuthorityEnvelope.Encode(envelope)).ToLowerInvariant());
    }

    /// <summary>
    /// Minimal ZLAU envelope reader/writer used only to pin the golden
    /// fixture's framing byte-exactly (magic, version, reserved flags,
    /// big-endian length prefix, opaque body, trailing CRC-32C). This
    /// mirrors the envelope ZLinkUserSpotAuthorityPayloadCodec and
    /// ZLinkActorAuthorityPayloadCodec already use, without decoding the
    /// instance-spot union body those production codecs do not yet cover.
    /// </summary>
    private readonly record struct DurableAuthorityEnvelope(
        byte Version,
        ushort Flags,
        byte[] Body)
    {
        private static ReadOnlySpan<byte> Magic => "ZLAU"u8;

        internal static bool TryDecode(
            ReadOnlySpan<byte> encoded,
            out DurableAuthorityEnvelope value)
        {
            value = default;
            if (encoded.Length < 4 + 1 + 2 + 4 + 4
                || !encoded[..4].SequenceEqual(Magic))
                return false;

            var version = encoded[4];
            var flags = (ushort)((encoded[5] << 8) | encoded[6]);
            var length = (uint)((encoded[7] << 24) | (encoded[8] << 16)
                                 | (encoded[9] << 8) | encoded[10]);
            var bodyStart = 11;
            if (checked(bodyStart + (int)length + 4) != encoded.Length)
                return false;

            var body = encoded.Slice(bodyStart, (int)length).ToArray();
            var checksumOffset = bodyStart + (int)length;
            var expectedChecksum = (uint)(
                (encoded[checksumOffset] << 24)
                | (encoded[checksumOffset + 1] << 16)
                | (encoded[checksumOffset + 2] << 8)
                | encoded[checksumOffset + 3]);
            if (expectedChecksum != ZLinkCrc32C.Compute(encoded[..checksumOffset]))
                return false;

            value = new DurableAuthorityEnvelope(version, flags, body);
            return true;
        }

        internal static byte[] Encode(DurableAuthorityEnvelope value)
        {
            var prefix = new byte[4 + 1 + 2 + 4 + value.Body.Length];
            Magic.CopyTo(prefix);
            prefix[4] = value.Version;
            prefix[5] = (byte)(value.Flags >> 8);
            prefix[6] = (byte)value.Flags;
            var length = (uint)value.Body.Length;
            prefix[7] = (byte)(length >> 24);
            prefix[8] = (byte)(length >> 16);
            prefix[9] = (byte)(length >> 8);
            prefix[10] = (byte)length;
            value.Body.CopyTo(prefix, 11);

            var checksum = ZLinkCrc32C.Compute(prefix);
            var result = new byte[prefix.Length + 4];
            prefix.CopyTo(result, 0);
            result[prefix.Length] = (byte)(checksum >> 24);
            result[prefix.Length + 1] = (byte)(checksum >> 16);
            result[prefix.Length + 2] = (byte)(checksum >> 8);
            result[prefix.Length + 3] = (byte)checksum;
            return result;
        }
    }
}
