using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Target-contract pin for checklist C-3 (store record golden fixture:
/// 21-location-runtime.md#2.4, 22-location-store-redis.md#7). This test
/// consumes golden/store-record-v1.json directly, independent of any
/// production opaque-record store codec -- no language has implemented the
/// zlink-location-v3 opaque record write path yet (checklist C-4). It stays
/// green today because sha256 key derivation and cmsgpack value decoding
/// need nothing from C-4. The cmsgpack member decoder below is written from
/// scratch against the MessagePack type table in
/// 22-location-store-redis.md#7 (str family for strings/bytes, unsigned int
/// family for expiresAtMs, bool for tombstone) rather than reusing
/// Zlink.Framework.Codecs.MessagePack, whose defaults are for application
/// payloads and are not proven to match Redis Lua cmsgpack byte-for-byte.
/// </summary>
public sealed class StoreRecordGoldenTests
{
    [Fact]
    public void StoreRecordGolden_key_derivation_and_value_vectors_decode_as_pinned()
    {
        var fixturePath = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/store-record-v1.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var root = fixture.RootElement;
        var prefix = root.GetProperty("prefixExample").GetString()!;
        var namespaceTag = root.GetProperty("namespace").GetString()!;

        foreach (var key in root.GetProperty("keyDerivation").EnumerateArray())
        {
            var preimage = Convert.FromHexString(key.GetProperty("preimageHex").GetString()!);
            var sha256Hex = Convert.ToHexString(SHA256.HashData(preimage)).ToLowerInvariant();
            Assert.Equal(key.GetProperty("sha256Hex").GetString(), sha256Hex);
            Assert.Equal(
                $"{prefix}:{namespaceTag}:{sha256Hex}",
                key.GetProperty("redisKey").GetString());
        }

        var relocationBlob = root.GetProperty("relocationBlob");
        var relocationBytes = Convert.FromHexString(relocationBlob.GetProperty("rawBytesHex").GetString()!);
        Assert.True(relocationBytes.Length > 0);
        // zlink-location-v3 and zlink-relocation-v1 are Redis Cluster hashtags
        // ({...}) so a multi-key EVAL script (record + sequence counter +
        // index, 22-location-store-redis.md#7) stays same-slot atomic under
        // Cluster; a brace-less key would let Cluster route the keys to
        // different slots.
        Assert.Equal(
            $"{prefix}:{{zlink-relocation-v1}}:blob:{relocationBlob.GetProperty("reference").GetString()}",
            relocationBlob.GetProperty("redisKey").GetString());
        Assert.Equal("{zlink-location-v3}:opaque", namespaceTag);

        foreach (var vector in root.GetProperty("valueVectors").GetProperty("genericOpaqueRecord").EnumerateArray())
        {
            var name = vector.GetProperty("name").GetString()!;
            var full = Convert.FromHexString(vector.GetProperty("fullValueHex").GetString()!);
            Assert.Equal(0x01, full[0]);
            var offset = 1;
            var decoded = DecodeOpaqueMember(full, ref offset);
            Assert.Equal(full.Length, offset);

            var expectedOriginalKey = vector.GetProperty("originalKey").GetString()!.Replace("\\u0000", "\0");
            Assert.Equal(expectedOriginalKey, decoded.OriginalKey);
            Assert.Equal(vector.GetProperty("jsonBytesHex").GetString(), Convert.ToHexString(decoded.RawBytes).ToLowerInvariant());
            Assert.Equal(vector.GetProperty("version").GetString(), decoded.Version);
            Assert.Equal(vector.GetProperty("expiresAtMs").GetString(), decoded.ExpiresAtMs.ToString());
            Assert.Equal(vector.GetProperty("tombstone").GetBoolean(), decoded.Tombstone);

            var member = Convert.FromHexString(vector.GetProperty("cmsgpackMemberHex").GetString()!);
            Assert.Equal(member, full[1..]);

            if (!vector.GetProperty("tombstone").GetBoolean())
            {
                using var parsed = JsonDocument.Parse(decoded.RawBytes);
                using var expectedDoc = JsonDocument.Parse(vector.GetProperty("decoded").GetRawText());
                Assert.Equal(
                    JsonSerializer.Serialize(expectedDoc.RootElement),
                    JsonSerializer.Serialize(parsed.RootElement));
            }
            else
            {
                Assert.Empty(decoded.RawBytes);
            }
        }
    }

    /// <summary>
    /// Checklist C-4 (dotnet store convergence): unlike the test above,
    /// which is a pure-function fixture self-test, this drives the actual
    /// production key builders (ZLinkProviderLocationRepository.OwnerKey/
    /// MeshKey/ClientServerKey/FanoutKey) and the production owner-lease
    /// JSON envelope encoder, so a regression in the real Redis provider's
    /// key derivation or envelope shape fails here.
    /// </summary>
    [Fact]
    public void StoreRecordGolden_dotnet_production_key_builders_match_golden()
    {
        var fixturePath = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/store-record-v1.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var root = fixture.RootElement;
        var byRecord = root.GetProperty("keyDerivation").EnumerateArray()
            .ToDictionary(
                entry => entry.GetProperty("record").GetString()!,
                entry => entry);

        AssertPreimage(
            byRecord["mesh-node-descriptor"],
            ZLinkProviderLocationRepository.MeshKey(
                "main",
                RoutingId.FromHex("01020304")).Value);
        AssertPreimage(
            byRecord["owner-lease"],
            ZLinkProviderLocationRepository.OwnerKey("owner-a").Value);
        AssertPreimage(
            byRecord["client-server-descriptor"],
            ZLinkProviderLocationRepository.ClientServerKey(
                "chat-channel",
                RoutingId.FromHex("01020304")).Value);
        AssertPreimage(
            byRecord["fanout-publisher-descriptor"],
            ZLinkProviderLocationRepository.FanoutKey(
                "chat-channel",
                RoutingId.FromHex("01020304")).Value);

        var ownerLeaseValueVector = root.GetProperty("valueVectors")
            .GetProperty("genericOpaqueRecord").EnumerateArray()
            .Single(vector =>
                vector.GetProperty("name").GetString() == "ownerLease-expired");
        var producedJson =
            ZLinkProviderLocationRepository.EncodeOwnerLeaseRecordForGoldenTest(
                "owner-a",
                5);
        Assert.Equal(
            ownerLeaseValueVector.GetProperty("jsonBytesHex").GetString(),
            Convert.ToHexString(producedJson).ToLowerInvariant());
    }

    private static void AssertPreimage(JsonElement vector, string producedKeyValue)
    {
        var producedPreimage = Encoding.UTF8.GetBytes(producedKeyValue);
        Assert.Equal(
            vector.GetProperty("preimageHex").GetString(),
            Convert.ToHexString(producedPreimage).ToLowerInvariant());
        var sha256Hex = Convert.ToHexString(SHA256.HashData(producedPreimage))
            .ToLowerInvariant();
        Assert.Equal(vector.GetProperty("sha256Hex").GetString(), sha256Hex);
    }

    private readonly record struct OpaqueMember(
        string OriginalKey, byte[] RawBytes, string Version, ulong ExpiresAtMs, bool Tombstone);

    private static OpaqueMember DecodeOpaqueMember(byte[] bytes, ref int offset)
    {
        var count = ReadArrayHead(bytes, ref offset);
        if (count != 5) throw new InvalidDataException($"invalid opaque member arity: {count}");
        var originalKey = Encoding.UTF8.GetString(ReadStr(bytes, ref offset));
        var rawBytes = ReadStr(bytes, ref offset);
        var version = Encoding.UTF8.GetString(ReadStr(bytes, ref offset));
        var expiresAtMs = ReadUint(bytes, ref offset);
        var tombstone = ReadBool(bytes, ref offset);
        return new OpaqueMember(originalKey, rawBytes, version, expiresAtMs, tombstone);
    }

    private static byte NextByte(byte[] bytes, ref int offset) => bytes[offset++];

    private static int ReadArrayHead(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if ((tag & 0xf0) == 0x90) return tag & 0x0f;
        if (tag == 0xdc)
        {
            return (NextByte(bytes, ref offset) << 8) | NextByte(bytes, ref offset);
        }
        if (tag == 0xdd)
        {
            var v = 0;
            for (var i = 0; i < 4; i++) v = (v << 8) | NextByte(bytes, ref offset);
            return v;
        }
        throw new InvalidDataException($"invalid msgpack array tag: {tag}");
    }

    private static byte[] ReadStr(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        int length;
        if ((tag & 0xe0) == 0xa0)
        {
            length = tag & 0x1f;
        }
        else if (tag == 0xd9)
        {
            length = NextByte(bytes, ref offset);
        }
        else if (tag == 0xda)
        {
            length = (NextByte(bytes, ref offset) << 8) | NextByte(bytes, ref offset);
        }
        else if (tag == 0xdb)
        {
            length = 0;
            for (var i = 0; i < 4; i++) length = (length << 8) | NextByte(bytes, ref offset);
        }
        else
        {
            throw new InvalidDataException($"invalid msgpack str tag: {tag}");
        }
        var value = bytes[offset..(offset + length)];
        offset += length;
        return value;
    }

    private static ulong ReadUint(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if ((tag & 0x80) == 0) return tag;
        if (tag == 0xcc) return NextByte(bytes, ref offset);
        if (tag == 0xcd) return (ulong)((NextByte(bytes, ref offset) << 8) | NextByte(bytes, ref offset));
        if (tag == 0xce)
        {
            ulong v = 0;
            for (var i = 0; i < 4; i++) v = (v << 8) | NextByte(bytes, ref offset);
            return v;
        }
        if (tag == 0xcf)
        {
            ulong v = 0;
            for (var i = 0; i < 8; i++) v = (v << 8) | NextByte(bytes, ref offset);
            return v;
        }
        throw new InvalidDataException($"invalid msgpack uint tag: {tag}");
    }

    private static bool ReadBool(byte[] bytes, ref int offset)
    {
        var tag = NextByte(bytes, ref offset);
        if (tag == 0xc2) return false;
        if (tag == 0xc3) return true;
        throw new InvalidDataException($"invalid msgpack bool tag: {tag}");
    }
}
