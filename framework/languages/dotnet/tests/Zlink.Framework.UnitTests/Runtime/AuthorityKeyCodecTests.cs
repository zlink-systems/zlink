using System.Text;
using System.Text.Json;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class AuthorityKeyCodecTests
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    [Fact]
    public void AuthorityKeyCodec_matches_the_shared_golden_fixture()
    {
        var fixturePath = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/authority-key-v1.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));

        foreach (var item in fixture.RootElement.GetProperty("cases").EnumerateArray())
        {
            var identity = StrictUtf8.GetString(
                Convert.FromHexString(item.GetProperty("identityHex").GetString()!));
            var expected = item.GetProperty("encoded").GetString()!;
            var kind = item.GetProperty("objectKind").GetString();

            if (kind == "actor")
            {
                var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(identity);
                Assert.Equal(expected, key.Value);
                Assert.True(ZLinkActorAuthorityPayloadCodec.TryGetActorId(key, out var decoded));
                Assert.Equal(identity, decoded);
            }
            else
            {
                var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(identity);
                Assert.Equal(expected, key.Value);
                Assert.True(ZLinkUserSpotAuthorityPayloadCodec.TryGetSpotId(key, out var decoded));
                Assert.Equal(identity, decoded);
            }
        }
    }

    [Theory]
    [InlineData("zla1:a:07:user%3A42")]
    [InlineData("zla1:a:+7:user%3A42")]
    [InlineData("zla1:a:7:user%3a42")]
    [InlineData("zla1:a:1:%41")]
    [InlineData("zla1:a:7:user:42")]
    [InlineData("zla1:a:7:user 42")]
    [InlineData("zla1:a:2:%C3%28")]
    [InlineData("zla1:a:8:user%3A42")]
    [InlineData("zla1:a:0:")]
    public void AuthorityKeyCodec_rejects_noncanonical_or_corrupt_actor_keys(
        string encoded)
    {
        var key = new ZLinkAuthorityKey(encoded);

        Assert.False(ZLinkActorAuthorityPayloadCodec.TryGetActorId(key, out var actorId));
        Assert.Equal(string.Empty, actorId);
        Assert.Throws<InvalidDataException>(() =>
            ZLinkAuthorityKeyCodec.DecodeActor(key));
    }

    [Fact]
    public void AuthorityKeyCodec_enforces_the_identity_byte_limit()
    {
        var maximum = new string('x', byte.MaxValue);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(maximum);

        Assert.Equal($"zla1:a:{byte.MaxValue}:{maximum}", key.Value);
        Assert.Equal(maximum, ZLinkAuthorityKeyCodec.DecodeActor(key));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(maximum + "x"));
    }

    [Fact]
    public void AuthorityKeyCodec_rejects_invalid_utf8_input_and_nul_spot_ids()
    {
        Assert.Throws<ArgumentException>(() =>
            ZLinkActorAuthorityPayloadCodec.AuthorityKey("\uD800"));
        Assert.False(ZLinkUserSpotAuthorityPayloadCodec.TryGetSpotId(
            new ZLinkAuthorityKey("zla1:s:1:%00"),
            out var spotId));
        Assert.Equal(string.Empty, spotId);
    }
}
