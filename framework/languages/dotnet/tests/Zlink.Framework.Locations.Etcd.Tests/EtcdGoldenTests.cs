using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using Google.Protobuf;

namespace Zlink.Framework.Locations.Etcd.Tests;

public sealed class EtcdGoldenTests
{
    private static JsonDocument Golden() =>
        JsonDocument.Parse(
            File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "location-store-etcd-v1.json"))
        );

    private static ByteString Hex(JsonElement value, string property) =>
        ByteString.CopyFrom(Convert.FromHexString(value.GetProperty(property).GetString()!));

    [Fact]
    public void All_key_clock_metadata_and_version_vectors_match_the_common_format()
    {
        using var document = Golden();
        var root = document.RootElement;
        foreach (var vector in root.GetProperty("keyVectors").EnumerateArray())
        {
            var ns = vector.GetProperty("namespace").GetString()!;
            var key = Hex(vector, "frameworkKeyHex");
            Assert.Equal(Hex(vector, "metadataKeyHex"), EtcdEncoding.Key(ns, (byte)'m', key));
            Assert.Equal(Hex(vector, "payloadKeyHex"), EtcdEncoding.Key(ns, (byte)'p', key));
            Assert.Equal(
                Hex(vector, "clockKeyHex"),
                EtcdEncoding.Key(ns, (byte)'c', ByteString.Empty)
            );
        }
        foreach (var vector in root.GetProperty("clockVectors").EnumerateArray())
        {
            var value = long.Parse(
                vector.GetProperty("value").GetString()!,
                CultureInfo.InvariantCulture
            );
            Assert.Equal(Hex(vector, "hex"), EtcdEncoding.Clock(value));
            Assert.Equal(value, EtcdEncoding.DecodeClock(Hex(vector, "hex")));
        }
        foreach (var vector in root.GetProperty("metadataVectors").EnumerateArray())
        {
            var expiry = long.Parse(
                vector.GetProperty("expiresAtMs").GetString()!,
                CultureInfo.InvariantCulture
            );
            var hasExpiry = vector.GetProperty("hasExpiry").GetBoolean();
            var length = vector.GetProperty("payloadLength").GetInt32();
            Assert.Equal(
                Hex(vector, "metadataHex"),
                EtcdEncoding.Metadata(hasExpiry, expiry, length)
            );
            var decoded = EtcdEncoding.DecodeMetadata(Hex(vector, "metadataHex"));
            Assert.Equal(hasExpiry, decoded.HasExpiry);
            Assert.Equal(expiry, decoded.ExpiresAt);
            Assert.Equal(length, decoded.PayloadLength);
        }
        foreach (
            var vector in root.GetProperty("storeVersions").GetProperty("accepted").EnumerateArray()
        )
        {
            var value = vector.GetProperty("value").GetString()!;
            Assert.Equal(
                value,
                EtcdEncoding.Version(
                    ulong.Parse(
                        vector.GetProperty("clusterIdDecimal").GetString()!,
                        CultureInfo.InvariantCulture
                    ),
                    long.Parse(
                        vector.GetProperty("modRevision").GetString()!,
                        CultureInfo.InvariantCulture
                    )
                )
            );
            Assert.True(EtcdEncoding.DecodeVersion(value).HasValue);
        }
        foreach (
            var vector in root.GetProperty("storeVersions").GetProperty("rejected").EnumerateArray()
        )
            Assert.False(EtcdEncoding.DecodeVersion(vector.GetString()!).HasValue);
    }

    [Fact]
    public async Task Every_corrupt_record_vector_is_a_store_failure_through_exact_read()
    {
        using var document = Golden();
        foreach (var vector in document.RootElement.GetProperty("corruptRecords").EnumerateArray())
        {
            await using var server = await FakeEtcdServer.StartAsync();
            server.Clock();
            if (vector.TryGetProperty("clockHex", out _))
                server.Seed(FakeEtcdServer.Key("test", "c"), Hex(vector, "clockHex").ToByteArray());
            else
            {
                var metadataRevision = vector.TryGetProperty("metadataModRevision", out var mr)
                    ? long.Parse(mr.GetString()!, CultureInfo.InvariantCulture)
                    : 10L;
                var payloadRevision = vector.TryGetProperty("payloadModRevision", out var pr)
                    ? long.Parse(pr.GetString()!, CultureInfo.InvariantCulture)
                    : 10L;
                if (vector.GetProperty("metadataHex").ValueKind != JsonValueKind.Null)
                    server.Seed(
                        FakeEtcdServer.Key("test", "m", "key"),
                        Hex(vector, "metadataHex").ToByteArray(),
                        metadataRevision
                    );
                if (vector.GetProperty("payloadHex").ValueKind != JsonValueKind.Null)
                    server.Seed(
                        FakeEtcdServer.Key("test", "p", "key"),
                        Hex(vector, "payloadHex").ToByteArray(),
                        payloadRevision
                    );
            }
            await using var store = server.CreateStore();
            await Assert.ThrowsAnyAsync<IOException>(() => store.ReadAsync(new("key")).AsTask());
        }
    }

    [Fact]
    public void Cursor_carries_maximum_keys_and_prefixes_within_the_SPI_bound_and_expires_monotonically()
    {
        var cursor = new EtcdCursor();
        var ns = new string('n', 64);
        var prefix = ByteString.CopyFrom(new byte[1024]);
        var snapshot = new EtcdSnapshot(
            ulong.MaxValue,
            long.MaxValue,
            EtcdEncoding.MaxTime,
            Stopwatch.GetTimestamp(),
            ByteString.CopyFrom(new byte[1024])
        );
        var encoded = cursor.Encode(snapshot, ns, prefix);
        Assert.InRange(encoded.Length, 1, 4096);
        Assert.Equal(snapshot, cursor.Decode(encoded, ns, prefix));
        Assert.Null(cursor.Decode(encoded, ns + "x", prefix));
        Assert.Null(cursor.Decode(encoded, ns, ByteString.CopyFromUtf8("other")));
        Assert.Null(new EtcdCursor().Decode(encoded, ns, prefix));
        var expired = snapshot with
        {
            Started = Stopwatch.GetTimestamp() - Stopwatch.Frequency * 301,
        };
        Assert.Null(cursor.Decode(cursor.Encode(expired, ns, prefix), ns, prefix));
        var tampered = (encoded[0] == 'A' ? "B" : "A") + encoded[1..];
        Assert.Null(cursor.Decode(tampered, ns, prefix));
    }
}
