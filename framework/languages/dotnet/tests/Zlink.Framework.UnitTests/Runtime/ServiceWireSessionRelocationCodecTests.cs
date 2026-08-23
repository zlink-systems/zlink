using System.Reflection;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceWireSessionRelocationCodecTests
{
    [Fact]
    public void Commands_42_through_44_match_the_shared_golden_vectors()
    {
        var seal = Seal();
        AssertGoldenRoundTrip(
            "sessionRelocationSeal",
            seal,
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal);

        AssertGoldenRoundTrip(
            "sessionRelocationSealed",
            new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                seal.RelocationId,
                seal.Coordinator,
                seal.Actor,
                seal.Session),
            ZLinkServiceWireCodec.EncodeSessionRelocationSealed,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed);

        AssertGoldenRoundTrip(
            "sessionRelocationRouteCommit",
            Route(
                2,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                    11, 12, RoutingId.From("target"), 4)),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute);

        AssertGoldenRoundTrip(
            "sessionRelocationRouteAbort",
            Route(
                1,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(
                    11)),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute);
    }

    [Fact]
    public void Session_relocation_DTOs_exclude_sequence_high_water_and_reply()
    {
        Assert.Equal(
            new[] { "RelocationId", "Coordinator", "Actor", "Session" },
            PublicPropertyNames<
                ZLinkServiceWireCodec.SessionRelocationSealedRecord>());
        Assert.Equal(
            new[]
            {
                "Action",
                "PreviousAuthorityOwnerGeneration",
                "TargetAuthorityOwnerGeneration",
                "TargetNodeRid",
                "TargetNodeGeneration",
                "CurrentAuthorityOwnerGeneration"
            },
            PublicPropertyNames<
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord>());

        var nestedTypeNames = typeof(ZLinkServiceWireCodec)
            .GetNestedTypes(BindingFlags.NonPublic)
            .Select(static type => type.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("SessionRelocationRoutedRecord", nestedTypeNames);
        Assert.DoesNotContain("SessionRelocationRouteResult", nestedTypeNames);

        var methodNames = typeof(ZLinkServiceWireCodec)
            .GetMethods(BindingFlags.Static | BindingFlags.NonPublic)
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("EncodeSessionRelocationRouted", methodNames);
        Assert.DoesNotContain("TryDecodeSessionRelocationRouted", methodNames);
    }

    [Fact]
    public void Session_relocation_roles_follow_the_exact_source_target_contract()
    {
        var seal = Seal();
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(
                seal with { SenderRole = 3 }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(
                seal with { SenderRole = 2 }));

        var commit = Route(
            2,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11, 12, RoutingId.From("target"), 4));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                commit with { SenderRole = 3 }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                commit with { SenderRole = 1 }));

        var abort = Route(
            1,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(11));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                abort with { SenderRole = 3 }));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(
                abort with { SenderRole = 2 }));
    }

    [Fact]
    public void Reserved_command_45_is_not_accepted_as_session_route()
    {
        var encoded = ReadGolden("sessionRelocationRouteCommit");
        encoded[3] = 45;

        Assert.False(ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(
            encoded, out _, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.UnknownCommand, error);
        Assert.DoesNotContain("SessionRelocationRouted",
            Enum.GetNames<ServiceWireConstants.Command>());
    }

    private delegate bool TryDecode<T>(ReadOnlySpan<byte> bytes, out T value,
        out ZLinkServiceWireCodec.DecodeError error);

    private static void AssertGoldenRoundTrip<T>(string name, T value,
        Func<T, byte[]> encode, TryDecode<T> decode)
    {
        var encoded = encode(value);
        Assert.Equal(ReadGolden(name), encoded);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(value, decoded);
        Assert.Equal(encoded, encode(decoded));

        Assert.False(decode(encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField,
            truncated);
        Assert.False(decode([.. encoded, 0], out _, out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);

        var forbiddenFlag = encoded.ToArray();
        forbiddenFlag[4] = 1;
        Assert.False(decode(forbiddenFlag, out _, out var flagError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.ForbiddenFlag,
            flagError);
    }

    private static string[] PublicPropertyNames<T>() =>
        typeof(T).GetProperties(BindingFlags.Instance | BindingFlags.Public)
            .Select(static property => property.Name)
            .ToArray();

    private static ZLinkServiceWireCodec.SessionRelocationSealRecord Seal()
    {
        var actor = new ZLinkServiceWireCodec.SessionActorIdentityRecord(
            "actor-1", 5);
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
