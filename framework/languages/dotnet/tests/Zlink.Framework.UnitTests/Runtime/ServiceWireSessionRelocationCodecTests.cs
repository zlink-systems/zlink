using System.Reflection;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceWireSessionRelocationCodecTests
{
    [Fact]
    public async Task Malformed_command_42_records_invalid_frame_dispatch_error()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "malformed-seal");
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var activities =
            new System.Collections.Concurrent.ConcurrentQueue<System.Diagnostics.Activity>();
        using var listener = new System.Diagnostics.ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (
                ref System.Diagnostics.ActivityCreationOptions<System.Diagnostics.ActivityContext> _
            ) => System.Diagnostics.ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Enqueue,
        };
        System.Diagnostics.ActivitySource.AddActivityListener(listener);
        node.SetFlowTracer(new ZLinkMessageFlowTracer(options));

        var sourceRid = RoutingId.From("malformed-command-42-source");
        var malformed = new byte[] { 0x5a, 0x4d, 0x01, 42, 0x00 };
        var receive = typeof(ZLinkManagedMeshNode).GetMethod(
            "ProcessInfrastructureControl",
            BindingFlags.NonPublic | BindingFlags.Instance
        )!;
        var handled = receive.Invoke(
            node,
            [
                sourceRid,
                1UL,
                null,
                new[] { Systems.Zlink.Message.From(malformed) },
                malformed,
                null,
                null,
            ]
        );

        Assert.Equal(false, handled);
        var error = Assert.Single(
            activities.Where(item => Equals(item.GetTagItem("event_id"), "zlink.dispatch_error"))
        );
        Assert.Equal("failed", error.GetTagItem("outcome"));
        Assert.Equal("stream", error.GetTagItem("surface"));
        Assert.Equal("control", error.GetTagItem("message_kind"));
        Assert.Equal("invalid_frame", error.GetTagItem("reason"));
        Assert.Equal("drop", error.GetTagItem("action"));
        Assert.Equal(sourceRid.ToHex(), error.GetTagItem("source_rid"));
    }

    [Fact]
    public async Task Seal_without_descriptor_records_location_unavailable_drop()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "seal-no-descriptor");
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var events =
            new System.Collections.Concurrent.ConcurrentQueue<System.Diagnostics.Activity>();
        using var listener = new System.Diagnostics.ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (
                ref System.Diagnostics.ActivityCreationOptions<System.Diagnostics.ActivityContext> _
            ) => System.Diagnostics.ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = events.Enqueue,
        };
        System.Diagnostics.ActivitySource.AddActivityListener(listener);
        node.SetFlowTracer(new ZLinkMessageFlowTracer(options));
        var seal = Seal();
        var sourceRid = RoutingId.From("seal-source-" + Guid.NewGuid().ToString("N"));
        seal = seal with
        {
            Coordinator = seal.Coordinator with { NodeRid = sourceRid },
            Actor = seal.Actor with { TargetNodeRid = sourceRid },
        };
        node.ProcessSessionRelocationSeal(seal.Coordinator.NodeRid, seal);
        var drop = Assert.Single(
            events.Where(item =>
                Equals(item.GetTagItem("packet_name"), "sessionRelocationSeal")
                && Equals(item.GetTagItem("source_rid"), sourceRid.ToHex())
            )
        );
        Assert.Equal("dropped", drop.GetTagItem("phase"));
        Assert.Equal("dropped", drop.GetTagItem("outcome"));
        Assert.Equal("stream", drop.GetTagItem("surface"));
        Assert.Equal("control", drop.GetTagItem("message_kind"));
        Assert.Equal("location_unavailable", drop.GetTagItem("reason"));
    }

    [Fact]
    public void Command_42_preserves_target_fence_and_rejects_empty_fields()
    {
        var record = Seal() with { TargetNodeGeneration = ulong.MaxValue };
        var encoded = ZLinkServiceWireCodec.EncodeSessionRelocationSeal(record);
        Assert.Equal(
            ulong.MaxValue,
            System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(encoded.AsSpan(^8))
        );
        var rid = record.TargetNodeRid.ToBytes().ToArray();
        Assert.Equal(rid, encoded.AsSpan(encoded.Length - 8 - rid.Length, rid.Length).ToArray());
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal(encoded, out var decoded, out _)
        );
        Assert.Equal(record, decoded);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(
                record with
                {
                    TargetNodeRid = default,
                }
            )
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(
                record with
                {
                    TargetNodeGeneration = 0,
                }
            )
        );
        encoded.AsSpan(^8).Clear();
        Assert.False(
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal(encoded, out _, out var error)
        );
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, error);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Participant_uses_first_command43_and_ignores_later_results(bool absentFirst)
    {
        var first = absentFirst
            ? ServiceWirePilotCodec.SessionSealResult.BindingAbsent
            : ServiceWirePilotCodec.SessionSealResult.Sealed;
        var later = absentFirst
            ? ServiceWirePilotCodec.SessionSealResult.Sealed
            : ServiceWirePilotCodec.SessionSealResult.BindingAbsent;
        var seal = Seal();
        var pending = new ZLinkManagedMeshNode.PendingSessionRelocationSeal(
            System.Security.Cryptography.SHA256.HashData(
                ZLinkServiceWireCodec.EncodeSessionRelocationSeal(seal)
            ),
            seal
        );
        var response = new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
            seal.RelocationId,
            seal.Coordinator,
            seal.Actor,
            seal.Session,
            first
        );
        Assert.False(
            pending.TryComplete(
                response with
                {
                    Actor = response.Actor with { OwnerLeaseGeneration = 9 },
                }
            )
        );
        Assert.False(pending.Completion.Task.IsCompleted);
        Assert.True(pending.TryComplete(response));
        Assert.False(pending.TryComplete(response with { RelocationId = new(8, 9) }));
        Assert.False(
            pending.TryComplete(
                response with
                {
                    Coordinator = response.Coordinator with { OwnerId = "changed" },
                }
            )
        );
        Assert.False(
            pending.TryComplete(
                response with
                {
                    Actor = response.Actor with { OwnerLeaseGeneration = 9 },
                }
            )
        );
        Assert.False(
            pending.TryComplete(
                response with
                {
                    Session = response.Session with { BindingGeneration = 9 },
                }
            )
        );
        Assert.True(pending.TryComplete(response with { SealResult = later }));
        Assert.Equal(response, await pending.Completion.Task);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Late_command43_validates_fields_for_pending_and_terminal(bool terminal)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "session-seal-replies");
        await using var monitor = node.OpenMonitor();
        var request = Seal();
        var response = new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
            request.RelocationId,
            request.Coordinator,
            request.Actor,
            request.Session
        );
        var pending = new ZLinkManagedMeshNode.PendingSessionRelocationSeal(
            System.Security.Cryptography.SHA256.HashData(
                ZLinkServiceWireCodec.EncodeSessionRelocationSeal(request)
            ),
            request
        );
        Assert.True(pending.TryComplete(response));
        var flags = BindingFlags.NonPublic | BindingFlags.Instance;
        var keyType = typeof(ZLinkManagedMeshNode).GetNestedType(
            "PendingSessionRelocationKey",
            BindingFlags.NonPublic
        )!;
        var key = keyType
            .GetMethods(BindingFlags.NonPublic | BindingFlags.Static)
            .Single(method =>
                method.Name == "Create"
                && method.GetParameters()[1].ParameterType == request.GetType()
            )
            .Invoke(null, [request.Session.SessionOwnerNodeRid, request])!;
        var field = terminal
            ? "_sessionRelocationSealReplyTerminals"
            : "_pendingSessionRelocationSeals";
        var entries = (System.Collections.IDictionary)
            typeof(ZLinkManagedMeshNode).GetField(field, flags)!.GetValue(node)!;
        entries.Add(key, pending);
        var receive = typeof(ZLinkManagedMeshNode).GetMethod(
            "ProcessSessionRelocationSealed",
            flags
        )!;
        receive.Invoke(
            node,
            [
                request.Session.SessionOwnerNodeRid,
                response with
                {
                    SealResult = ServiceWirePilotCodec.SessionSealResult.BindingAbsent,
                },
            ]
        );
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        receive.Invoke(
            node,
            [
                request.Session.SessionOwnerNodeRid,
                response with
                {
                    Coordinator = response.Coordinator with { OwnerId = "changed" },
                },
            ]
        );
        receive.Invoke(
            node,
            [
                request.Session.SessionOwnerNodeRid,
                response with
                {
                    Actor = response.Actor with { OwnerLeaseGeneration = 9 },
                },
            ]
        );
        receive.Invoke(
            node,
            [
                request.Session.SessionOwnerNodeRid,
                response with
                {
                    Session = response.Session with { BindingGeneration = 9 },
                },
            ]
        );
        Assert.Equal(3UL, monitor.Status().ProtocolErrors);
        Assert.Equal(response, await pending.Completion.Task);
    }

    [Fact]
    public void Commands_42_through_44_match_the_shared_golden_vectors()
    {
        var golden = ServiceWirePilotCodec.DecodeSessionRelocationSeal42(
            ReadGolden("sessionRelocationSeal")
        );
        var seal = Seal() with
        {
            TargetNodeRid = RoutingId.From(golden.TargetNodeRid),
            TargetNodeGeneration = golden.TargetNodeGeneration,
        };
        AssertGoldenRoundTrip(
            "sessionRelocationSeal",
            seal,
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSeal
        );

        AssertGoldenRoundTrip(
            "sessionRelocationSealed",
            new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                seal.RelocationId,
                seal.Coordinator,
                seal.Actor,
                seal.Session
            ),
            ZLinkServiceWireCodec.EncodeSessionRelocationSealed,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed
        );

        AssertGoldenRoundTrip(
            "sessionRelocationRouteCommit",
            Route(
                2,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                    11,
                    12,
                    RoutingId.From("target"),
                    4,
                    5
                )
            ),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute
        );

        AssertGoldenRoundTrip(
            "sessionRelocationRouteAbort",
            Route(1, ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(11)),
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute,
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute
        );
    }

    [Fact]
    public void Session_relocation_DTOs_exclude_sequence_high_water_and_reply()
    {
        Assert.Equal(
            new[] { "RelocationId", "Coordinator", "Actor", "Session", "SealResult" },
            PublicPropertyNames<ZLinkServiceWireCodec.SessionRelocationSealedRecord>()
        );
        Assert.Equal(
            new[]
            {
                "Action",
                "PreviousAuthorityOwnerGeneration",
                "TargetAuthorityOwnerGeneration",
                "TargetNodeRid",
                "TargetNodeGeneration",
                "TargetOwnerLeaseGeneration",
                "CurrentAuthorityOwnerGeneration",
            },
            PublicPropertyNames<ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord>()
        );

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
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(seal with { SenderRole = 3 })
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationSeal(seal with { SenderRole = 2 })
        );

        var commit = Route(
            2,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                RoutingId.From("target"),
                4,
                5
            )
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(commit with { SenderRole = 3 })
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(commit with { SenderRole = 1 })
        );

        var abort = Route(1, ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(11));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(abort with { SenderRole = 3 })
        );
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSessionRelocationRoute(abort with { SenderRole = 2 })
        );
    }

    [Fact]
    public void Reserved_command_45_is_not_accepted_as_session_route()
    {
        var encoded = ReadGolden("sessionRelocationRouteCommit");
        encoded[3] = 45;

        Assert.False(
            ZLinkServiceWireCodec.TryDecodeSessionRelocationRoute(encoded, out _, out var error)
        );
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.UnknownCommand, error);
        Assert.DoesNotContain(
            "SessionRelocationRouted",
            Enum.GetNames<ServiceWireConstants.Command>()
        );
    }

    private delegate bool TryDecode<T>(
        ReadOnlySpan<byte> bytes,
        out T value,
        out ZLinkServiceWireCodec.DecodeError error
    );

    private static void AssertGoldenRoundTrip<T>(
        string name,
        T value,
        Func<T, byte[]> encode,
        TryDecode<T> decode
    )
    {
        var encoded = encode(value);
        Assert.Equal(ReadGolden(name), encoded);
        Assert.True(decode(encoded, out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(value, decoded);
        Assert.Equal(encoded, encode(decoded));

        Assert.False(decode(encoded[..^1], out _, out var truncated));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField, truncated);
        Assert.False(decode([.. encoded, 0], out _, out var trailing));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailing);

        var forbiddenFlag = encoded.ToArray();
        forbiddenFlag[4] = 1;
        Assert.False(decode(forbiddenFlag, out _, out var flagError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.ForbiddenFlag, flagError);
    }

    private static string[] PublicPropertyNames<T>() =>
        typeof(T)
            .GetProperties(BindingFlags.Instance | BindingFlags.Public)
            .Select(static property => property.Name)
            .ToArray();

    private static ZLinkServiceWireCodec.SessionRelocationSealRecord Seal()
    {
        var actor = new ZLinkServiceWireCodec.SessionActorIdentityRecord("actor-1", 5);
        return new ZLinkServiceWireCodec.SessionRelocationSealRecord(
            new ZLinkServiceWireCodec.RelocationWireId(7, 9),
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator",
                3,
                RoutingId.From("source"),
                2,
                "store-v17"
            ),
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                actor,
                RoutingId.From("source"),
                2,
                11,
                13
            ),
            new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                RoutingId.From("source"),
                2,
                "session-owner",
                8,
                RoutingId.From("session"),
                6
            ),
            RoutingId.From("target"),
            3
        );
    }

    private static ZLinkServiceWireCodec.SessionRelocationRouteRecord Route(
        byte senderRole,
        ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord route
    )
    {
        var seal = Seal();
        return new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            seal.RelocationId,
            seal.Coordinator,
            senderRole,
            seal.Actor.Actor,
            seal.Session,
            route
        );
    }

    [Theory]
    [InlineData("sessionRelocationSealed", "sealed")]
    [InlineData("sessionRelocationBindingAbsent", "bindingAbsent")]
    public void SealResult_RuntimeRoundTripsBothResults(string name, string result)
    {
        var bytes = ReadGolden(name);
        var context = ServiceWireCodec.DecodeContext.Empty;
        var generated = ServiceWireCodec.DecodeSessionRelocationSealed43(bytes, context);
        Assert.Equal(
            result == "sealed"
                ? ServiceWireCodec.SessionSealResult.Sealed
                : ServiceWireCodec.SessionSealResult.BindingAbsent,
            generated.SealResult
        );
        Assert.Equal(
            bytes,
            ServiceWireCodec.EncodeSessionRelocationSealed43(generated, context)[0]
        );
        var pilot = ServiceWirePilotCodec.DecodeSessionRelocationSealed43(bytes);
        Assert.Equal(bytes, ServiceWirePilotCodec.EncodeSessionRelocationSealed43(pilot));
        var accepted = ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed(
            bytes,
            out var runtimeRecord,
            out var error
        );
        Assert.True(accepted);
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(pilot.SealResult, runtimeRecord.SealResult);
        Assert.Equal(bytes, ZLinkServiceWireCodec.EncodeSessionRelocationSealed(runtimeRecord));
    }

    [Theory]
    [InlineData("missingSealResult")]
    [InlineData("zeroSealResult")]
    [InlineData("unknownSealResult")]
    [InlineData("trailingSealResult")]
    public void SealResult_MalformedBytesAreRejected(string name)
    {
        var bytes = ReadGolden(name, "malformed");
        Assert.ThrowsAny<Exception>(() =>
            ServiceWireCodec.DecodeSessionRelocationSealed43(
                bytes,
                ServiceWireCodec.DecodeContext.Empty
            )
        );
        Assert.ThrowsAny<Exception>(() =>
            ServiceWirePilotCodec.DecodeSessionRelocationSealed43(bytes)
        );
        Assert.False(ZLinkServiceWireCodec.TryDecodeSessionRelocationSealed(bytes, out _, out _));
    }

    private static byte[] ReadGolden(string name, string section = "canonical")
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/session-relocation-barrier-v1.json",
            frameworkRoot
        );
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document
            .RootElement.GetProperty(section)
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }
}
