using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceWireSessionRelocationCodecTests
{
    [Fact]
    public void Commands_42_through_45_match_the_shared_golden_vectors()
    {
        var relocation = new ZLinkServiceWireCodec.RelocationWireId(7, 9);
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            "coordinator",
            3,
            RoutingId.From("source"),
            2,
            "store-v17");
        var actor = new ZLinkServiceWireCodec.SessionActorIdentityRecord(
            "actor-1",
            5);
        var actorFence = new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
            actor,
            RoutingId.From("source"),
            2,
            11,
            13);
        var session = new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
            RoutingId.From("source"),
            2,
            "session-owner",
            8,
            RoutingId.From("session"),
            6);

        AssertGoldenRoundTrip(
            "sessionRelocationSeal",
            new ZLinkServiceWireCodec.SessionRelocationSealRecord(
                relocation,
                coordinator,
                1,
                actorFence,
                session),
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal);
        AssertGoldenRoundTrip(
            "sessionRelocationSealed",
            new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                relocation,
                coordinator,
                actorFence,
                session,
                41),
            ZLinkServiceWireCodec.EncodeSessionRelocationSealed,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed);

        var commit = ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord
            .Commit(11, 12, RoutingId.From("target"), 4, 41);
        AssertGoldenRoundTrip(
            "sessionRelocationRouteCommit",
            new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
                relocation,
                coordinator,
                2,
                actor,
                session,
                commit),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute);
        AssertGoldenRoundTrip(
            "sessionRelocationRoutedCommit",
            new ZLinkServiceWireCodec.SessionRelocationRoutedRecord(
                relocation,
                coordinator,
                actor,
                session,
                ZLinkServiceWireCodec.SessionRelocationRouteAction.Commit,
                ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
                12,
                41),
            ZLinkServiceWireCodec.EncodeSessionRelocationRouted,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRouted);

        var abort = ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord
            .Abort(11);
        AssertGoldenRoundTrip(
            "sessionRelocationRouteAbort",
            new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
                relocation,
                coordinator,
                1,
                actor,
                session,
                abort),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute);
        AssertGoldenRoundTrip(
            "sessionRelocationRoutedAbort",
            new ZLinkServiceWireCodec.SessionRelocationRoutedRecord(
                relocation,
                coordinator,
                actor,
                session,
                ZLinkServiceWireCodec.SessionRelocationRouteAction.Abort,
                ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
                11,
                41),
            ZLinkServiceWireCodec.EncodeSessionRelocationRouted,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRouted);
    }

    [Fact]
    public void Session_relocation_codec_rejects_role_union_and_frame_shape_changes()
    {
        var seal = Seal();
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(
                seal with { SenderRole = 2 }));

        var commit = Route(
            2,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                RoutingId.From("target"),
                4,
                41));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                commit with { SenderRole = 1 }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                commit with
                {
                    Route = ZLinkServiceWireCodec
                        .SessionRelocationRouteUpdateRecord.Commit(
                            12,
                            12,
                            RoutingId.From("target"),
                            4,
                            41)
                }));

        var encoded = ZLinkServiceWireCodec.EncodeSessionRelocationRoute(commit);
        Assert.False(ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(
            encoded[..^1],
            out _,
            out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField, truncated);
        Assert.False(ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(
            [.. encoded, 0],
            out _,
            out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);
    }

    [Fact]
    public void Command_43_preserves_a_zero_owner_high_water()
    {
        var seal = Seal();
        var value = new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
            seal.RelocationId,
            seal.Coordinator,
            seal.Actor,
            seal.Session,
            0);
        var encoded = ZLinkServiceWireCodec.EncodeSessionRelocationSealed(value);

        Assert.True(ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed(
            encoded,
            out var decoded,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(0UL, decoded.LastAcceptedSessionSequence);
    }

    private delegate bool TryDecode<T>(
        ReadOnlySpan<byte> bytes,
        out T value,
        out ZLinkServiceWireCodec.DecodeError error);

    private static void AssertGoldenRoundTrip<T>(
        string name,
        T value,
        Func<T, byte[]> encode,
        TryDecode<T> decode)
    {
        var encoded = encode(value);
        Assert.Equal(ReadGolden(name), encoded);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(value, decoded);
        Assert.Equal(encoded, encode(decoded));
    }

    private static ZLinkServiceWireCodec.SessionRelocationSealRecord Seal()
    {
        var actor = new ZLinkServiceWireCodec.SessionActorIdentityRecord(
            "actor-1",
            5);
        return new ZLinkServiceWireCodec.SessionRelocationSealRecord(
            new ZLinkServiceWireCodec.RelocationWireId(7, 9),
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator",
                3,
                RoutingId.From("source"),
                2,
                "store-v17"),
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                actor,
                RoutingId.From("source"),
                2,
                11,
                13),
            new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                RoutingId.From("source"),
                2,
                "session-owner",
                8,
                RoutingId.From("session"),
                6));
    }

    private static ZLinkServiceWireCodec.SessionRelocationRouteRecord Route(
        byte senderRole,
        ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord route)
    {
        var seal = Seal();
        return new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            seal.RelocationId,
            seal.Coordinator,
            senderRole,
            seal.Actor.Actor,
            seal.Session,
            route);
    }

    private static byte[] ReadGolden(string name)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/session-relocation-barrier-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document.RootElement.GetProperty("canonical")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }
}
