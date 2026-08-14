using System.Buffers.Binary;
using System.Diagnostics;
using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class ServiceRuntimeFoundationTests
{
    [Theory]
    [InlineData(1024)]
    [InlineData(17 * 1024 * 1024)]
    public void FrameworkMultipart_Encodes_Without_A_RouteMesh_Message_Bound(
        int partLength)
    {
        var parts = new ReadOnlyMemory<byte>[] { new byte[partLength] };

        var expectedLength =
            ZLinkApplicationPayloadEnvelopeCodec.GetFrameworkMultipartEncodedLength(
                parts);
        var encoded = ZLinkApplicationPayloadEnvelopeCodec
            .EncodeFrameworkMultipart(parts);

        Assert.Equal(expectedLength, encoded.LongLength);
        Assert.True(
            ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                encoded,
                out var decoded));
        try
        {
            Assert.Equal(partLength, Assert.Single(decoded).Size);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(decoded);
        }
    }

    [Fact]
    public void MessageFollow_RoundTripsVersionedInfrastructureRecord()
    {
        var source = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowActorKind,
            "actor-1",
            11,
            RoutingId.From("source-node"),
            12,
            13,
            14);
        var target = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowActorKind,
            "actor-1",
            11,
            RoutingId.From("target-node"),
            15,
            16,
            17);
        var record = new ZLinkServiceWireCodec.MessageFollowRecord(
            source,
            target,
            1,
            3,
            4096,
            new MeshOperationId(21, 22),
            23);

        var encoded = ZLinkServiceWireCodec.EncodeMessageFollow(record);

        Assert.Equal((byte)50, encoded[3]);
        Assert.Equal((byte)1, encoded[5]);
        Assert.Equal(
            (uint)(encoded.Length - 10),
            BinaryPrimitives.ReadUInt32BigEndian(encoded.AsSpan(6)));
        Assert.True(ZLinkServiceWireCodec.TryDecodeMessageFollow(
            encoded,
            out var decoded,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(record, decoded);
        Assert.Equal(
            encoded,
            ZLinkServiceWireCodec.EncodeMessageFollow(decoded));
    }

    [Fact]
    public void MessageFollow_RejectsFramingViolationsAndAcceptsVolumeMetadata()
    {
        var route = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowSpotKind,
            "spot-1",
            3,
            RoutingId.From("node"),
            4,
            5,
            6);
        var encoded = ZLinkServiceWireCodec.EncodeMessageFollow(
            new ZLinkServiceWireCodec.MessageFollowRecord(
                route,
                route,
                8,
                1024,
                16 * 1024 * 1024,
                new MeshOperationId(7, 8),
                0));

        var wrongVersion = encoded.ToArray();
        wrongVersion[5] = 2;
        Assert.False(ZLinkServiceWireCodec.TryDecodeMessageFollow(
            wrongVersion,
            out _,
            out var wrongVersionError));
        Assert.Equal(
            ZLinkServiceWireCodec.DecodeError.UnsupportedVersion,
            wrongVersionError);

        var trailing = encoded.Concat(new byte[] { 0 }).ToArray();
        Assert.False(ZLinkServiceWireCodec.TryDecodeMessageFollow(
            trailing,
            out _,
            out var trailingError));
        Assert.Equal(
            ZLinkServiceWireCodec.DecodeError.InvalidField,
            trailingError);

        var volumeRecord = new ZLinkServiceWireCodec.MessageFollowRecord(
            route,
            route,
            1,
            uint.MaxValue,
            uint.MaxValue,
            new MeshOperationId(1, 1),
            0);
        Assert.True(ZLinkServiceWireCodec.TryDecodeMessageFollow(
            ZLinkServiceWireCodec.EncodeMessageFollow(volumeRecord),
            out var decodedVolumeRecord,
            out var volumeError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, volumeError);
        Assert.Equal(volumeRecord, decodedVolumeRecord);
    }

    [Fact]
    public void BoundSessionReplaced_MatchesCanonicalWireFixture()
    {
        var record = new ZLinkServiceWireCodec.BoundSessionReplacedRecord(
            new ZLinkServiceWireCodec.BoundSessionReplacedActorAuthority(
                "actor-a",
                1,
                RoutingId.From("actor-owner"),
                2,
                3,
                4),
            new ZLinkServiceWireCodec.BoundSessionReplacedRetiredSession(
                RoutingId.From("session-owner"),
                5,
                "session-runtime",
                6,
                RoutingId.From("session-a"),
                7));

        var expected = new byte[]
        {
            90, 77, 1, 51, 0,
            7, 97, 99, 116, 111, 114, 45, 97,
            0, 0, 0, 0, 0, 0, 0, 1,
            11, 97, 99, 116, 111, 114, 45, 111, 119, 110, 101, 114,
            0, 0, 0, 0, 0, 0, 0, 2,
            0, 0, 0, 0, 0, 0, 0, 3,
            0, 0, 0, 0, 0, 0, 0, 4,
            13, 115, 101, 115, 115, 105, 111, 110, 45, 111, 119, 110, 101, 114,
            0, 0, 0, 0, 0, 0, 0, 5,
            15, 115, 101, 115, 115, 105, 111, 110, 45, 114, 117, 110, 116, 105, 109, 101,
            0, 0, 0, 0, 0, 0, 0, 6,
            9, 115, 101, 115, 115, 105, 111, 110, 45, 97,
            0, 0, 0, 0, 0, 0, 0, 7
        };

        var encoded = ZLinkServiceWireCodec.EncodeBoundSessionReplaced(record);
        Assert.Equal(expected, encoded);
        Assert.True(ZLinkServiceWireCodec.TryDecodeBoundSessionReplaced(
            encoded,
            out var decoded,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(record, decoded);

        var trailing = encoded.Append((byte)0).ToArray();
        Assert.False(ZLinkServiceWireCodec.TryDecodeBoundSessionReplaced(
            trailing,
            out _,
            out var trailingError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailingError);
    }

    [Fact]
    public async Task BoundSessionReplaced_Transport_Requires_The_Admitted_Authority_And_Retired_Owner_Lifecycles()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var authority = new ZLinkManagedMeshNode(context, "actors");
        await using var retiredOwner = new ZLinkManagedMeshNode(context, "actors");
        await using var monitor = retiredOwner.OpenMonitor();
        var suffix = Guid.NewGuid().ToString("N");
        var authorityRid = RoutingId.From($"authority-{suffix}");
        var retiredOwnerRid = RoutingId.From($"retired-{suffix}");
        var authorityEndpoint = $"inproc://authority-{suffix}";
        var retiredOwnerEndpoint = $"inproc://retired-{suffix}";
        var notifications = 0;

        authority.SetRoutingId(authorityRid);
        authority.SetBind(authorityEndpoint);
        authority.ConnectPeer(retiredOwnerEndpoint, retiredOwnerRid);
        retiredOwner.SetRoutingId(retiredOwnerRid);
        retiredOwner.SetBind(retiredOwnerEndpoint);
        retiredOwner.SetBoundSessionReplacedNotificationHandler(
            (_, _) => Interlocked.Increment(ref notifications));
        retiredOwner.Start();
        authority.Start();

        await WaitUntilAsync(() =>
            authority.Status().AdmittedPeerCount == 1
            && retiredOwner.Status().AdmittedPeerCount == 1);
        var authorityGeneration = Assert.Single(retiredOwner.Peers())
            .LifecycleGeneration;
        var retiredOwnerGeneration = Assert.Single(authority.Peers())
            .LifecycleGeneration;

        ZLinkServiceWireCodec.BoundSessionReplacedRecord Record(
            RoutingId sourceRid,
            ulong sourceGeneration,
            ulong ownerGeneration) => new(
            new ZLinkServiceWireCodec.BoundSessionReplacedActorAuthority(
                "actor-a",
                ObjectGeneration: 17,
                sourceRid,
                sourceGeneration,
                ExpectedAuthorityOwnerGeneration: 19,
                ExpectedOwnerLeaseGeneration: 23),
            new ZLinkServiceWireCodec.BoundSessionReplacedRetiredSession(
                retiredOwnerRid,
                ownerGeneration,
                "retired-owner",
                SessionOwnerLeaseGeneration: 29,
                RoutingId.From("retired-session"),
                RetiredBindingGeneration: 31));

        Assert.True(authority.TrySendBoundSessionReplacedNotification(
            retiredOwnerRid,
            Record(authorityRid, authorityGeneration, retiredOwnerGeneration)));
        await WaitUntilAsync(() => Volatile.Read(ref notifications) == 1);

        var protocolErrors = monitor.Status().ProtocolErrors;
        Assert.True(authority.TrySendBoundSessionReplacedNotification(
            retiredOwnerRid,
            Record(RoutingId.From("forged-authority"), authorityGeneration, retiredOwnerGeneration)));
        await WaitUntilAsync(() => monitor.Status().ProtocolErrors > protocolErrors);
        Assert.Equal(1, Volatile.Read(ref notifications));

        protocolErrors = monitor.Status().ProtocolErrors;
        Assert.True(authority.TrySendBoundSessionReplacedNotification(
            retiredOwnerRid,
            Record(authorityRid, authorityGeneration + 1, retiredOwnerGeneration)));
        await WaitUntilAsync(() => monitor.Status().ProtocolErrors > protocolErrors);
        Assert.Equal(1, Volatile.Read(ref notifications));

        protocolErrors = monitor.Status().ProtocolErrors;
        Assert.True(authority.TrySendBoundSessionReplacedNotification(
            retiredOwnerRid,
            Record(authorityRid, authorityGeneration, retiredOwnerGeneration + 1)));
        await WaitUntilAsync(() => monitor.Status().ProtocolErrors > protocolErrors);
        Assert.Equal(1, Volatile.Read(ref notifications));
    }

    [Fact]
    public void GeneratedLivenessFixtures_DecodeWithExactErrors()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/service-decoder-fixtures-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));

        foreach (var fixture in document.RootElement.GetProperty("canonical").EnumerateArray())
        {
            var bytes = fixture.GetProperty("bytes").EnumerateArray()
                .Select(static item => item.GetByte()).ToArray();
            Assert.True(ZLinkServiceWireCodec.TryDecodeLiveness(
                bytes, out var record, out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal(fixture.GetProperty("commandId").GetByte(), (byte)record.Command);
            Assert.Equal(0x0102030405060708UL, record.ProbeId);
            Assert.Equal(bytes, ZLinkServiceWireCodec.EncodeLiveness(record.Command, record.ProbeId));
        }

        foreach (var fixture in document.RootElement.GetProperty("malformed").EnumerateArray())
        {
            var bytes = fixture.GetProperty("bytes").EnumerateArray()
                .Select(static item => item.GetByte()).ToArray();
            Assert.False(ZLinkServiceWireCodec.TryDecodeLiveness(
                bytes, out _, out var error));
            Assert.Equal(ExpectedError(fixture.GetProperty("error").GetString()!), error);
        }
    }

    [Fact]
    public void WireCodec_UsesGeneratedConstantsAndUtf8Bounds()
    {
        var encoded = ZLinkServiceWireCodec.EncodeLiveness(
            ServiceWireConstants.Command.LivenessProbe,
            1);
        Assert.Equal(ServiceWireConstants.Magic0, encoded[0]);
        Assert.Equal(ServiceWireConstants.Magic1, encoded[1]);
        Assert.Equal(ServiceWireConstants.WireMajor, encoded[2]);

        var text = ZLinkServiceWireCodec.EncodeText("가");
        Assert.Equal(3, text[0]);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeText(new string('a', 256)));
    }

    [Fact]
    public void InfrastructureControl_AllowsOnlyBoundedCommands()
    {
        using var liveness = Message.From(
            ZLinkServiceWireCodec.EncodeLiveness(
                ServiceWireConstants.Command.LivenessProbe,
                1));
        Assert.True(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [liveness],
            out var command));
        Assert.Equal(ServiceWireConstants.Command.LivenessProbe, command);

        using var application = Message.From(
            ZLinkServiceWireCodec.EncodeApplication(
                ServiceWireConstants.Command.NodeSend,
                0,
                null,
                false));
        Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [application],
            out _));

        using var applicationReply = Message.From(
            ZLinkServiceWireCodec.EncodeReply(1, 0, 0));
        using var replyPayload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                new ReadOnlyMemory<byte>[] { new byte[] { 1, 2, 3 } }));
        Assert.True(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [applicationReply, replyPayload],
            out command));
        Assert.Equal(ServiceWireConstants.Command.Reply, command);

        using var largeReplyPayload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                new ReadOnlyMemory<byte>[]
                {
                    new byte[(256 * 1024) + 1]
                }));
        Assert.True(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [applicationReply, largeReplyPayload],
            out command));
        Assert.Equal(ServiceWireConstants.Command.Reply, command);

        using var invalidReplyPayload = Message.From(new byte[] { 1, 2, 3 });
        Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [applicationReply, invalidReplyPayload],
            out _));

        using var extraReplyPayload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                new ReadOnlyMemory<byte>[] { new byte[] { 4 } }));
        using var extraReplyPart = Message.From(new byte[] { 5 });
        Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [applicationReply, extraReplyPayload, extraReplyPart],
            out _));

        using var relocationData = Message.From(new byte[]
        {
            ServiceWireConstants.Magic0,
            ServiceWireConstants.Magic1,
            ServiceWireConstants.WireMajor,
            (byte)ServiceWireConstants.Command.RelocationData,
            0
        });
        Assert.True(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [relocationData],
            out command));
        Assert.Equal(ServiceWireConstants.Command.RelocationData, command);

        var largeRelocationData = new byte[(256 * 1024) + 1];
        largeRelocationData[0] = ServiceWireConstants.Magic0;
        largeRelocationData[1] = ServiceWireConstants.Magic1;
        largeRelocationData[2] = ServiceWireConstants.WireMajor;
        largeRelocationData[3] = (byte)ServiceWireConstants.Command.RelocationData;
        using var largeRelocation = Message.From(largeRelocationData);
        Assert.True(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [largeRelocation],
            out command));
        Assert.Equal(ServiceWireConstants.Command.RelocationData, command);

        Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [relocationData, extraReplyPayload],
            out _));

        var oversizedBytes = new byte[(256 * 1024) + 1];
        oversizedBytes[0] = ServiceWireConstants.Magic0;
        oversizedBytes[1] = ServiceWireConstants.Magic1;
        oversizedBytes[2] = ServiceWireConstants.WireMajor;
        oversizedBytes[3] = (byte)ServiceWireConstants.Command.Hello;
        using var oversized = Message.From(oversizedBytes);
        Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
            [oversized],
            out _));

        var tooMany = Enumerable.Range(0, 65)
            .Select(_ => Message.From(ReadOnlySpan<byte>.Empty))
            .ToArray();
        try
        {
            Assert.False(ZLinkManagedMeshNode.IsAllowedInfrastructureControl(
                tooMany,
                out _));
        }
        finally
        {
            foreach (var part in tooMany)
                part.Dispose();
        }
    }

    [Fact]
    public void RouteAdmission_RoundTripsDeterministicDescriptor()
    {
        var channels = new Dictionary<string, uint>(StringComparer.Ordinal)
        {
            ["worker"] = 75,
            ["admin"] = 0
        };

        var encoded = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            "orders",
            "tcp://127.0.0.1:7070",
            17,
            23,
            channels);

        var lifecycleOffset = 10
            + 1 + "orders"u8.Length
            + 1 + ZLinkServiceSecurityIdentity.Plaintext.Length;
        Assert.Equal(
            17UL,
            BinaryPrimitives.ReadUInt64BigEndian(encoded.AsSpan(lifecycleOffset)));

        Assert.True(ZLinkServiceWireCodec.TryDecodeRouteAdmission(
            encoded,
            out var command,
            out var admission,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(ServiceWireConstants.Command.Hello, command);
        Assert.Equal("orders", admission.MeshName);
        Assert.Equal("tcp://127.0.0.1:7070", admission.AdvertisedEndpoint);
        Assert.Equal(17UL, admission.LifecycleGeneration);
        Assert.Equal(23UL, admission.DescriptorRevision);
        Assert.Equal(0U, admission.Channels["admin"]);
        Assert.Equal(75U, admission.Channels["worker"]);
        Assert.Equal(ZLinkServiceSecurityIdentity.Plaintext, admission.SecurityIdentity);
        Assert.Equal(1, admission.RuntimeState);
        Assert.Equal(0, admission.ApplicationVersion);
        Assert.Equal(0, admission.ObjectRole);
        Assert.Equal(100U, admission.PlacementWeight);
        Assert.Equal(10_000U, admission.ActiveCapacityLimit);
        Assert.Equal(128U, admission.PendingCapacityLimit);
        Assert.Equal(0U, admission.ActiveCapacityUsed);
        Assert.Equal(0U, admission.PendingCapacityUsed);
        Assert.Equal(
            new byte[] { 1, 2, 6, 7, 8, 9, 10, 11, 12 },
            admission.ExtensionFields.Keys);
        Assert.Equal(
            encoded,
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                admission.MeshName,
                admission.AdvertisedEndpoint,
                admission.LifecycleGeneration,
                admission.DescriptorRevision,
                admission.Channels,
                admission.ObjectRole,
                admission.RuntimeState,
                admission.SecurityIdentity));
    }

    [Fact]
    public void RouteAdmission_PreservesUnknownExtensionFields()
    {
        var encoded = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            "orders",
            "tcp://127.0.0.1:7070",
            17,
            23,
            new Dictionary<string, uint>(StringComparer.Ordinal)
            {
                ["worker"] = 75
            });
        var extended = AppendDescriptorExtension(encoded, 13, [0xaa, 0xbb]);

        Assert.True(ZLinkServiceWireCodec.TryDecodeRouteAdmission(
            extended,
            out _,
            out var admission,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(new byte[] { 0xaa, 0xbb }, admission.ExtensionFields[13]);
        Assert.Equal(extended.AsSpan(10).ToArray(), admission.DescriptorBytes);
    }

    [Fact]
    public void RouteAdmission_Preserves_Object_Role()
    {
        var encoded = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            "orders",
            "tcp://127.0.0.1:7070",
            17,
            23,
            new Dictionary<string, uint>(StringComparer.Ordinal),
            (byte)ZLinkMeshNodeObjectRole.Client);

        var admission = DecodeAdmission(encoded);

        Assert.Equal((byte)ZLinkMeshNodeObjectRole.Client, admission.ObjectRole);
    }

    [Fact]
    public void AdmissionGuard_ValidatesRevisionAndImmutableFieldsBeforeMutation()
    {
        var channels = new Dictionary<string, uint>(StringComparer.Ordinal)
        {
            ["worker"] = 75
        };
        var current = DecodeAdmission(
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                "orders",
                "tcp://127.0.0.1:7070",
                17,
                23,
                channels));

        Assert.Equal(
            ZLinkServiceAdmissionDecision.Idempotent,
            ZLinkServiceAdmissionGuard.Evaluate(
                current,
                ServiceWireConstants.Command.Update,
                current));

        var newer = DecodeAdmission(
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Update,
                "orders",
                "tcp://127.0.0.1:7070",
                17,
                24,
                new Dictionary<string, uint>(StringComparer.Ordinal)
                {
                    ["worker"] = 25
                }));
        Assert.Equal(
            ZLinkServiceAdmissionDecision.Accept,
            ZLinkServiceAdmissionGuard.Evaluate(
                current,
                ServiceWireConstants.Command.Update,
                newer));

        var sameRevisionDifferentBytes = DecodeAdmission(
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Update,
                "orders",
                "tcp://127.0.0.1:7070",
                17,
                23,
                new Dictionary<string, uint>(StringComparer.Ordinal)
                {
                    ["worker"] = 25
                }));
        Assert.Equal(
            ZLinkServiceAdmissionDecision.Reject,
            ZLinkServiceAdmissionGuard.Evaluate(
                current,
                ServiceWireConstants.Command.Update,
                sameRevisionDifferentBytes));

        var immutableMutationBytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Update,
            "orders",
            "tcp://127.0.0.1:7070",
            17,
            24,
            channels);
        var securityOffset = FindSequence(immutableMutationBytes, "default"u8);
        "changed"u8.CopyTo(immutableMutationBytes.AsSpan(securityOffset));
        var immutableMutation = DecodeAdmission(immutableMutationBytes);
        Assert.Equal(
            ZLinkServiceAdmissionDecision.Reject,
            ZLinkServiceAdmissionGuard.Evaluate(
                current,
                ServiceWireConstants.Command.Update,
                immutableMutation));
        Assert.True(ZLinkServiceAdmissionGuard.MatchesExpectedRoute(
            "tcp://127.0.0.1:7070",
            ZLinkServiceSecurityIdentity.Plaintext,
            current.LifecycleGeneration,
            current));
        Assert.False(ZLinkServiceAdmissionGuard.MatchesExpectedRoute(
            "tcp://127.0.0.1:7071",
            ZLinkServiceSecurityIdentity.Plaintext,
            current.LifecycleGeneration,
            current));
        Assert.False(ZLinkServiceAdmissionGuard.MatchesExpectedRoute(
            "tcp://127.0.0.1:7070",
            "different-security",
            current.LifecycleGeneration,
            current));
        Assert.False(ZLinkServiceAdmissionGuard.MatchesExpectedRoute(
            "tcp://127.0.0.1:7070",
            ZLinkServiceSecurityIdentity.Plaintext,
            current.LifecycleGeneration + 1,
            current));
        Assert.True(ZLinkServiceAdmissionGuard.MatchesExpectedTransportRoute(
            "tcp://127.0.0.1:7070",
            ZLinkServiceSecurityIdentity.Plaintext,
            ZLinkServiceSecurityIdentity.Plaintext,
            current.LifecycleGeneration,
            current));
        Assert.False(ZLinkServiceAdmissionGuard.MatchesExpectedTransportRoute(
            "tcp://127.0.0.1:7070",
            "tls:configured",
            ZLinkServiceSecurityIdentity.Plaintext,
            current.LifecycleGeneration,
            current with { SecurityIdentity = "tls:configured" }));

        Assert.Equal(
            ZLinkServiceAdmissionDecision.Reject,
            ZLinkServiceAdmissionGuard.Evaluate(
                null,
                ServiceWireConstants.Command.Update,
                newer));
    }

    [Fact]
    public void AdmissionGuard_SelectsOnePhysicalConnectionForExactPeerIncarnation()
    {
        var smaller = RoutingId.From("mesh-a");
        var larger = RoutingId.From("mesh-z");

        Assert.Equal(
            ZLinkServiceDuplicateConnectionDecision.KeepCurrent,
            ZLinkServiceAdmissionGuard.SelectConnection(
                smaller,
                larger,
                currentLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Outbound,
                "out:tcp://mesh-z:0001",
                incomingLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Inbound,
                "in:tcp://mesh-z:0002"));
        Assert.Equal(
            ZLinkServiceDuplicateConnectionDecision.UseIncoming,
            ZLinkServiceAdmissionGuard.SelectConnection(
                larger,
                smaller,
                currentLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Outbound,
                "out:tcp://mesh-a:0002",
                incomingLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Inbound,
                "in:tcp://mesh-a:0001"));
        Assert.Equal(
            ZLinkServiceDuplicateConnectionDecision.KeepCurrent,
            ZLinkServiceAdmissionGuard.SelectConnection(
                smaller,
                larger,
                currentLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Outbound,
                "out:tcp://mesh-z:0001",
                incomingLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Outbound,
                "out:tcp://mesh-z:0002"));
        Assert.Equal(
            ZLinkServiceDuplicateConnectionDecision.NotDuplicate,
            ZLinkServiceAdmissionGuard.SelectConnection(
                smaller,
                larger,
                currentLifecycleGeneration: 17,
                ZLinkServiceConnectionDirection.Outbound,
                "out:tcp://mesh-z:0001",
                incomingLifecycleGeneration: 19,
                ZLinkServiceConnectionDirection.Inbound,
                "in:tcp://mesh-z:0002"));
    }

    [Fact]
    public void SpotPeerMonitoring_MapsSignedAdmittedChannelWeight()
    {
        var peer = new MeshNodePeer(
            ConnectionIntentId: 7,
            Source: MeshPeerSource.Manual,
            State: MeshPeerState.Admitted,
            RoutingId: RoutingId.From("mesh-z"),
            LifecycleGeneration: 17,
            DescriptorRevision: 3,
            Endpoint: "tcp://127.0.0.1:7002",
            ChannelCount: 2,
            LastError: 0,
            LastChangedMs: 41);

        var mapped = peer.ToFramework(
            "tcp://127.0.0.1:7001",
            new MeshPeerChannel("orders", 75));

        Assert.Equal("orders", mapped.ChannelName);
        Assert.Equal(75, mapped.Weight);
        Assert.IsType<int>(mapped.Weight);
        Assert.Equal("tcp://127.0.0.1:7001", mapped.LocalEndpoint);
        Assert.NotEqual((int)peer.ChannelCount, mapped.Weight);
    }

    [Fact]
    public void ApplicationAndReplyRecords_RoundTripExactTerminalFields()
    {
        var request = ZLinkServiceWireCodec.EncodeApplication(
            ServiceWireConstants.Command.ChannelRequest,
            41,
            "worker",
            hasMetadata: true);
        Assert.True(ZLinkServiceWireCodec.TryDecodeApplication(
            request,
            out var application,
            out var requestError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, requestError);
        Assert.Equal(41UL, application.Correlation);
        Assert.Equal("worker", application.ChannelName);
        Assert.True(application.HasMetadata);

        var reply = ZLinkServiceWireCodec.EncodeReply(41, -3, 19);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            reply,
            out var terminal,
            out var replyError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, replyError);
        Assert.Equal(41UL, terminal.Correlation);
        Assert.Equal(-3, terminal.TerminalResult);
        Assert.Equal(19U, terminal.FailureCode);
    }

    [Fact]
    public void Liveness_RetransmitsOutstandingProbeAndExtendsOnlyOnExactAck()
    {
        var frequency = Stopwatch.Frequency;
        var liveness = new ZLinkServiceLiveness(0);

        Assert.False(liveness.TryGetProbe(5 * frequency - 1, out _));
        Assert.True(liveness.TryGetProbe(5 * frequency, out var firstProbe));
        Assert.NotEqual(0UL, firstProbe);
        Assert.True(liveness.TryGetProbe(10 * frequency, out var retransmit));
        Assert.Equal(firstProbe, retransmit);
        Assert.False(liveness.Acknowledge(firstProbe + 1, 11 * frequency));
        Assert.Equal(15 * frequency, liveness.DeadlineTimestamp);
        Assert.True(liveness.Acknowledge(firstProbe, 11 * frequency));
        Assert.Equal(26 * frequency, liveness.DeadlineTimestamp);
        Assert.False(liveness.IsExpired(26 * frequency - 1));
        Assert.True(liveness.IsExpired(26 * frequency));

        Assert.True(liveness.TryGetProbe(15 * frequency, out var nextProbe));
        Assert.NotEqual(firstProbe, nextProbe);
    }

    [Fact]
    public async Task ManagedNode_ReadmitsPeerAfterLivenessExpiry()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var local = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var localRid = RoutingId.From($"liveness-local-{suffix}");
        var remoteRid = RoutingId.From($"liveness-remote-{suffix}");
        var localEndpoint = $"inproc://liveness-local-{suffix}";
        var remoteEndpoint = $"inproc://liveness-remote-{suffix}";

        local.SetRoutingId(localRid);
        local.SetBind(localEndpoint);
        local.ConnectPeer(remoteEndpoint, remoteRid);

        await using (var firstRemote = new ZLinkManagedMeshNode(context, "orders"))
        {
            firstRemote.SetRoutingId(remoteRid);
            firstRemote.SetBind(remoteEndpoint);
            firstRemote.Start();
            local.Start();

            await WaitUntilAsync(() =>
                local.Status().AdmittedPeerCount == 1
                && firstRemote.Status().AdmittedPeerCount == 1);
        }

        // Mesh liveness expires after 15 seconds without an acknowledgement.
        // Reusing the same endpoint and RID verifies that the old admission
        // does not make the replacement Hello idempotent forever.
        await Task.Delay(TimeSpan.FromSeconds(16));

        await using var replacement = new ZLinkManagedMeshNode(context, "orders");
        replacement.SetRoutingId(remoteRid);
        replacement.SetBind(remoteEndpoint);
        replacement.ConnectPeer(localEndpoint, localRid);
        replacement.Start();

        await WaitUntilAsync(() =>
            local.Status().AdmittedPeerCount == 1
            && replacement.Status().AdmittedPeerCount == 1);
    }

    [Fact]
    public async Task EntrySpotUsesItsMeshNodeLifecycleAsTheDescriptorFence()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        node.SetRoutingId(RoutingId.From("orders-entry-owner"));

        var entry = Assert.IsType<ZLinkManagedSpot>(node.EntrySpot());
        var lifecycle = node.Status().LifecycleGeneration;

        Assert.NotEqual<ulong>(0, lifecycle);
        Assert.Equal(lifecycle, entry.LifecycleGeneration);
        Assert.Equal(lifecycle, entry.AuthorityOwnerGeneration);
    }

    [Fact]
    public async Task BackendEntrySpotRoutingId_RekeysManagedSpotToDescriptorIdentity()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        node.SetRoutingId(RoutingId.From("orders-entry-owner"));
        await using var backend = new ZLinkBackendSpotNodeWrapper(node);

        var entry = backend.EntrySpot();
        var descriptorEntrySpotId =
            "orders-entry-00000000-0000-4000-8000-000000000001";

        entry.SetRoutingId(ZLinkSpotId.ToNativeRoutingId(descriptorEntrySpotId));

        var native = node.GetOrCreateSpot(descriptorEntrySpotId, out var created);
        Assert.False(created);
        Assert.Equal(
            ZLinkSpotId.ToNativeRoutingId(descriptorEntrySpotId),
            native.RoutingId);
        Assert.Equal(
            ZLinkSpotId.ToNativeRoutingId(descriptorEntrySpotId),
            entry.RoutingId);
    }

    [Fact]
    public async Task DispatchPump_RekeysEntrySpotStateWithDescriptorIdentity()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        node.SetRoutingId(RoutingId.From("orders-entry-owner"));
        await using var pump = new ZLinkMeshDispatchPump(
            node,
            new ZLinkMeshCompletionTable());

        var previousSpotId = "orders-entry-owner";
        var descriptorEntrySpotId =
            "orders-entry-00000000-0000-4000-8000-000000000001";
        var state = pump.RegisterSpot(previousSpotId);

        pump.RekeySpot(previousSpotId, descriptorEntrySpotId, state);

        Assert.Same(state, pump.RegisterSpot(descriptorEntrySpotId));
        Assert.NotSame(state, pump.RegisterSpot(previousSpotId));
    }

    [Fact]
    public async Task ManagedNode_AdvertisesTheActualEndpointAssignedForPortZero()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        node.SetRoutingId(RoutingId.From("orders-port-zero"));
        node.SetBind("tcp://127.0.0.1:0");

        node.Start();

        var endpoint = node.Status().LocalEndpoint;
        Assert.StartsWith("tcp://127.0.0.1:", endpoint, StringComparison.Ordinal);
        Assert.False(endpoint.EndsWith(":0", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData(true, false, false)]
    [InlineData(false, true, false)]
    [InlineData(false, false, true)]
    public async Task ManagedNode_RejectsAdmissionThatDoesNotMatchDiscovery(
        bool mismatchEndpoint,
        bool mismatchSecurityIdentity,
        bool mismatchLifecycleGeneration)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "orders");
        await using var target = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"source-{suffix}");
        var targetRid = RoutingId.From($"target-{suffix}");
        var sourceEndpoint = $"inproc://source-{suffix}";
        var targetEndpoint = $"inproc://target-{suffix}";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        var sourceLifecycleGeneration = source.Status().LifecycleGeneration;
        target.SetPeerExpectation(
            sourceRid,
            mismatchEndpoint ? $"inproc://unexpected-{suffix}" : sourceEndpoint,
            mismatchSecurityIdentity
                ? "tls:unexpected"
                : ZLinkServiceSecurityIdentity.Plaintext,
            mismatchLifecycleGeneration
                ? sourceLifecycleGeneration == ulong.MaxValue
                    ? sourceLifecycleGeneration - 1
                    : sourceLifecycleGeneration + 1
                : sourceLifecycleGeneration);
        using var monitor = target.OpenMonitor();

        target.Start();
        source.Start();

        await WaitUntilAsync(() => monitor.Status().PeerRejected != 0);
        Assert.Equal(0u, target.Status().AdmittedPeerCount);
    }

    [Fact]
    public async Task ManagedNode_LocalAndTransportOperationsShareOneIdNamespace()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        var nodeRid = RoutingId.From("orders-operation-source");
        node.SetRoutingId(nodeRid);
        var localOperation = node.AllocateOperationId();

        using var requestPart = Message.From(new byte[] { 1 });
        Assert.Equal(
            SubmitResult.Ok,
            node.RequestToNode(
                nodeRid,
                [requestPart],
                out var transportOperation,
                TimeSpan.FromSeconds(1)));

        Assert.Equal(localOperation.High, transportOperation.High);
        Assert.Equal(localOperation.Low + 1, transportOperation.Low);
    }

    [Fact]
    public async Task ManagedNode_LocalRequestPublishesOneTerminalCompletion()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        var nodeRid = RoutingId.From("orders-1");
        node.SetRoutingId(nodeRid);

        using var requestPart = Message.From(new byte[] { 1, 2, 3 });
        Assert.Equal(
            SubmitResult.Ok,
            node.RequestToNode(
                nodeRid,
                [requestPart],
                out var operationId,
                TimeSpan.FromSeconds(1)));

        using var ready = new MeshReadyBatch();
        Assert.False(node.DrainReady(
            MeshReadyDomains.All,
            ready,
            RecvFlags.DontWait));
        Assert.Equal(1, ready.Count);

        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        var request = received[0];
        Assert.Equal(MeshRecordKind.NodeRequest, request.Kind);

        using var replyPart = Message.From(new byte[] { 4, 5, 6 });
        Assert.Equal(SubmitResult.Ok, request.Reply([replyPart]));
        Assert.Equal(SubmitResult.Ok, request.Reply([replyPart]));

        received.Reset();
        Assert.False(claim.Receive(received, RecvFlags.DontWait));
        claim.Dispose();
        ready.Reset();
        node.DrainReady(
            MeshReadyDomains.Infrastructure,
            ready,
            RecvFlags.DontWait);
        using var completionClaim = ready.TakeClaim(0);
        Assert.True(completionClaim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(1, received.Count);
        Assert.Equal(MeshRecordKind.Completion, received[0].Kind);
        Assert.Equal(operationId, received[0].OperationId);
        Assert.Equal((int)RequestResult.Ok, received[0].TerminalResult);
        Assert.False(completionClaim.Receive(received, RecvFlags.DontWait));
    }

    [Fact]
    public async Task ManagedNode_Status_RemainsReadable_DuringConcurrentQueueDrain()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "orders");
        var nodeRid = RoutingId.From("orders-status-race");
        node.SetRoutingId(nodeRid);
        using var stop = new CancellationTokenSource();
        Exception? statusFailure = null;

        var drainTask = Task.Run(async () =>
        {
            using var ready = new MeshReadyBatch();
            using var received = new MeshReceiveBatch();
            while (!stop.IsCancellationRequested)
            {
                ready.Reset();
                node.DrainReady(
                    MeshReadyDomains.Application,
                    ready,
                    RecvFlags.DontWait);
                for (var index = 0; index < ready.Count; index++)
                {
                    using var claim = ready.TakeClaim(index);
                    received.Reset();
                    claim.Receive(received, RecvFlags.DontWait);
                }
                await Task.Yield();
            }
        });
        var statusTask = Task.Run(() =>
        {
            try
            {
                for (var index = 0; index < 10_000; index++)
                    _ = node.Status();
            }
            catch (Exception exception)
            {
                statusFailure = exception;
            }
        });

        for (var index = 0; index < 2_000; index++)
        {
            using var part = Message.From(new byte[] { 1, 2, 3, 4 });
            Assert.Equal(
                SubmitResult.Ok,
                node.SendToNode(nodeRid, [part]));
        }

        await statusTask;
        stop.Cancel();
        await drainTask;

        Assert.Null(statusFailure);
        Assert.Equal(0UL, node.Status().PendingBytes);
    }

    [Theory]
    [InlineData(false, 4)]
    [InlineData(true, 4)]
    [InlineData(true, 17 * 1024 * 1024)]
    public async Task ManagedNodes_RemoteNodeAndChannelRequestsUseNativeReplyCompletion(
        bool channelRequest,
        int payloadLength)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "orders");
        await using var target = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"request-source-{suffix}");
        var targetRid = RoutingId.From($"request-target-{suffix}");
        var sourceEndpoint = $"inproc://request-source-{suffix}";
        var targetEndpoint = $"inproc://request-target-{suffix}";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        if (channelRequest)
            target.AddChannel("worker");
        target.Start();
        source.Start();

        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 1
                                   && target.Status().AdmittedPeerCount == 1);

        var requestPayload = Enumerable.Range(0, payloadLength)
            .Select(static index => checked((byte)(index % 251)))
            .ToArray();
        using var requestPart = Message.From(requestPayload);
        MeshOperationId operationId;
        var submit = channelRequest
            ? source.EntrySpot().RequestToChannel("worker", [requestPart], out operationId, TimeSpan.FromSeconds(3))
            : source.RequestToNode(targetRid, [requestPart], out operationId, TimeSpan.FromSeconds(3));
        Assert.Equal(SubmitResult.Ok, submit);

        using var targetReady = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            targetReady.Reset();
            target.DrainReady(MeshReadyDomains.Application, targetReady, RecvFlags.DontWait);
            return targetReady.Count == 1;
        });
        using var targetClaim = targetReady.TakeClaim(0);
        using var targetBatch = new MeshReceiveBatch();
        Assert.True(targetClaim.Receive(targetBatch, RecvFlags.DontWait));
        Assert.Equal(1, targetBatch.Count);
        var request = targetBatch[0];
        Assert.Equal(channelRequest ? MeshRecordKind.ChannelRequest : MeshRecordKind.NodeRequest, request.Kind);
        Assert.Equal(channelRequest ? MeshOperationKind.ChannelRequest : MeshOperationKind.NodeRequest, request.OperationKind);
        Assert.Equal(sourceRid, request.SourceNodeRid);
        Assert.Equal(operationId.Low, request.OperationId.Low);
        Assert.Equal(channelRequest ? "worker" : null, request.ChannelName);
        var receivedRequestParts = targetBatch.RetainMessage(0);
        try
        {
            Assert.Single(receivedRequestParts);
            Assert.Equal(requestPayload, receivedRequestParts[0].ToArray());
        }
        finally
        {
            foreach (var part in receivedRequestParts)
                part.Dispose();
        }

        using var replyPart = Message.From(new byte[] { 9, 8, 7 });
        Assert.Equal(SubmitResult.Ok, request.Reply([replyPart]));
        Assert.Equal(SubmitResult.InvalidState, request.Reply([replyPart]));

        using var sourceReady = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            sourceReady.Reset();
            source.DrainReady(MeshReadyDomains.Infrastructure, sourceReady, RecvFlags.DontWait);
            return sourceReady.Count == 1;
        });
        using var sourceClaim = sourceReady.TakeClaim(0);
        using var sourceBatch = new MeshReceiveBatch();
        Assert.True(sourceClaim.Receive(sourceBatch, RecvFlags.DontWait));
        Assert.Equal(1, sourceBatch.Count);
        var completion = sourceBatch[0];
        Assert.Equal(MeshRecordKind.Completion, completion.Kind);
        Assert.Equal(operationId, completion.OperationId);
        Assert.Equal(channelRequest ? MeshOperationKind.ChannelRequest : MeshOperationKind.NodeRequest, completion.OperationKind);
        Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
        Assert.Equal(0, completion.FailureErrno);
        var receivedReplyParts = sourceBatch.RetainMessage(0);
        try
        {
            Assert.Single(receivedReplyParts);
            Assert.Equal(new byte[] { 9, 8, 7 }, receivedReplyParts[0].ToArray());
        }
        finally
        {
            foreach (var part in receivedReplyParts)
                part.Dispose();
        }
        sourceBatch.Reset();
        Assert.False(sourceClaim.Receive(sourceBatch, RecvFlags.DontWait));
    }

    [Fact]
    public async Task AutoConnect_Cleans_Connecting_Peer_And_Preserves_Admitted_Peer()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var local = new ZLinkManagedMeshNode(context, "auto-cleanup");
        await using var remote = new ZLinkManagedMeshNode(context, "auto-cleanup");
        await using var localBackend = new ZLinkBackendSpotNodeWrapper(local);
        var suffix = Guid.NewGuid().ToString("N");
        var localRid = RoutingId.From($"auto-cleanup-local-{suffix}");
        var remoteRid = RoutingId.From($"auto-cleanup-remote-{suffix}");
        var staleRid = RoutingId.From($"auto-cleanup-stale-{suffix}");
        var localEndpoint = $"inproc://auto-cleanup-local-{suffix}";
        var remoteEndpoint = $"inproc://auto-cleanup-remote-{suffix}";
        var staleEndpoint = $"inproc://auto-cleanup-stale-{suffix}";

        local.SetRoutingId(localRid);
        local.SetBind(localEndpoint);
        local.ConnectPeer(remoteEndpoint, remoteRid);
        local.ConnectPeer(staleEndpoint, staleRid);
        remote.SetRoutingId(remoteRid);
        remote.SetBind(remoteEndpoint);
        remote.Start();
        local.Start();

        await WaitUntilAsync(() =>
            local.Peers().Any(peer =>
                peer.RoutingId == remoteRid
                && peer.State == MeshPeerState.Admitted)
            && local.Peers().Any(peer =>
                peer.RoutingId == staleRid
                && peer.State == MeshPeerState.Connecting));

        var admitted = Assert.Single(
            local.Peers(),
            peer => peer.RoutingId == remoteRid);
        var stale = Assert.Single(
            local.Peers(),
            peer => peer.RoutingId == staleRid);

        Assert.True(localBackend.DisconnectPeerBeforeAdmission(
            stale.RoutingId,
            stale.Endpoint,
            stale.LifecycleGeneration));

        Assert.DoesNotContain(
            local.Peers(),
            peer => peer.ConnectionIntentId == stale.ConnectionIntentId);
        var retained = Assert.Single(
            local.Peers(),
            peer => peer.ConnectionIntentId == admitted.ConnectionIntentId);
        Assert.Equal(MeshPeerState.Admitted, retained.State);
        Assert.False(localBackend.DisconnectPeerBeforeAdmission(
            retained.RoutingId,
            retained.Endpoint,
            retained.LifecycleGeneration));
        Assert.Contains(
            local.Peers(),
            peer => peer.ConnectionIntentId == retained.ConnectionIntentId
                    && peer.State == MeshPeerState.Admitted);
    }

    [Fact]
    public async Task AutoConnect_Stale_SameEndpoint_Cleanup_Preserves_Replacement_Transport()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var local = new ZLinkManagedMeshNode(context, "auto-replacement");
        await using var remote = new ZLinkManagedMeshNode(context, "auto-replacement");
        await using var localBackend = new ZLinkBackendSpotNodeWrapper(local);
        var suffix = Guid.NewGuid().ToString("N");
        var localRid = RoutingId.From($"auto-replacement-local-{suffix}");
        var oldRid = RoutingId.From($"auto-replacement-old-{suffix}");
        var replacementRid = RoutingId.From($"auto-replacement-new-{suffix}");
        var localEndpoint = $"inproc://auto-replacement-local-{suffix}";
        var remoteEndpoint = $"inproc://auto-replacement-remote-{suffix}";

        local.SetRoutingId(localRid);
        local.SetBind(localEndpoint);
        var oldIntent = local.ConnectPeer(remoteEndpoint, oldRid);
        var replacementIntent = local.ConnectPeer(remoteEndpoint, replacementRid);
        local.Start();

        var oldPeer = Assert.Single(
            local.Peers(),
            peer => peer.ConnectionIntentId == oldIntent);
        Assert.Contains(
            local.Peers(),
            peer => peer.ConnectionIntentId == replacementIntent
                    && peer.State == MeshPeerState.Connecting);

        Assert.True(localBackend.DisconnectPeerBeforeAdmission(
            oldRid,
            remoteEndpoint,
            lifecycleGeneration: 0));

        Assert.DoesNotContain(
            local.Peers(),
            peer => peer.ConnectionIntentId == oldPeer.ConnectionIntentId);
        Assert.Contains(
            local.Peers(),
            peer => peer.ConnectionIntentId == replacementIntent
                    && peer.State == MeshPeerState.Connecting);

        remote.SetRoutingId(replacementRid);
        remote.SetBind(remoteEndpoint);
        remote.Start();

        await WaitUntilAsync(() =>
            local.Peers().Any(peer =>
                peer.RoutingId == replacementRid
                && peer.State == MeshPeerState.Admitted)
            && remote.Peers().Any(peer =>
                peer.RoutingId == localRid
                && peer.State == MeshPeerState.Admitted));
    }

    [Fact]
    public async Task ManagedNode_Tcp_SameEndpoint_Replacement_RemainsAdmitted_Across_Repeated_Lifecycles()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var local = new ZLinkManagedMeshNode(context, "tcp-replacement");
        await using var localBackend = new ZLinkBackendSpotNodeWrapper(local);
        var suffix = Guid.NewGuid().ToString("N");
        var localRid = RoutingId.From($"tcp-replacement-local-{suffix}");
        var remoteEndpoint = string.Empty;
        ZLinkManagedMeshNode? remote = null;

        local.SetRoutingId(localRid);
        local.SetBind("tcp://127.0.0.1:0");
        local.Start();

        try
        {
            for (var generation = 0; generation < 3; generation++)
            {
                if (remote is not null)
                {
                    // The auto-connect owner removes the old intent before it
                    // installs the new RID at the same endpoint.
                    localBackend.DisconnectPeer(remoteEndpoint);
                    await WaitUntilAsync(() => !local.Peers().Any(peer =>
                        string.Equals(
                            peer.Endpoint,
                            remoteEndpoint,
                            StringComparison.Ordinal)));
                    await remote.DisposeAsync();
                    remote = null;
                }

                var remoteRid = RoutingId.From(
                    $"tcp-replacement-remote-{generation}-{suffix}");
                remote = new ZLinkManagedMeshNode(context, "tcp-replacement");
                remote.SetRoutingId(remoteRid);
                remote.SetBind(
                    generation == 0
                        ? "tcp://127.0.0.1:0"
                        : remoteEndpoint);
                remote.Start();
                remoteEndpoint = remote.Status().LocalEndpoint;

                localBackend.ConnectPeer(
                    remoteRid,
                    remoteEndpoint,
                    ZLinkServiceSecurityIdentity.Plaintext);

                var expectedRid = remoteRid;
                await WaitUntilAsync(() =>
                    local.Peers().Any(peer =>
                        peer.RoutingId == expectedRid
                        && peer.State == MeshPeerState.Admitted)
                    && remote!.Peers().Any(peer =>
                        peer.RoutingId == localRid
                        && peer.State == MeshPeerState.Admitted));
            }
        }
        finally
        {
            if (remote is not null)
                await remote.DisposeAsync();
        }
    }


    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ManagedNodes_RemoteSpotAndActorRequestsRequireOneNativeReplyCompletion(
        bool actorRequest)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "objects");
        await using var target = new ZLinkManagedMeshNode(context, "objects");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"stateful-source-{suffix}");
        var targetRid = RoutingId.From($"stateful-target-{suffix}");
        var sourceEndpoint = $"inproc://stateful-source-{suffix}";
        var targetEndpoint = $"inproc://stateful-target-{suffix}";
        source.SetRoutingId(sourceRid);
        source.ConnectPeer(targetEndpoint, targetRid);
        source.SetBind(sourceEndpoint);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        source.SetLocalOwnerLeaseGeneration(17);
        target.SetLocalOwnerLeaseGeneration(17);

        var actor = target.CreateActor($"actor-{suffix}");
        var spot = (ZLinkManagedSpot)target.GetOrCreateSpot($"spot-{suffix}", out _);
        Assert.True(target.TryGetActorAuthority(
            actor, out var actorAuthority, out var actorLease));
        source.ObserveActorAuthority(
            actor, target.Status().LifecycleGeneration, actorAuthority, actorLease);
        source.ObserveSpotAuthority(
            targetRid, spot.SpotId, spot.LifecycleGeneration,
            target.Status().LifecycleGeneration, spot.AuthorityOwnerGeneration, 17);
        target.Start();
        source.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        using var requestPart = Message.From(new byte[] { 3, 1, 4, 1, 5 });
        MeshOperationId operationId;
        var submit = actorRequest
            ? source.RequestToActor(
                actor, [requestPart], out operationId, TimeSpan.FromSeconds(3))
            : source.EntrySpot().RequestToSpot(
                targetRid, spot.SpotId, spot.LifecycleGeneration,
                [requestPart], out operationId, TimeSpan.FromSeconds(3));
        Assert.Equal(SubmitResult.Ok, submit);

        using var targetReady = new MeshReadyBatch();
        var requestReadyIndex = -1;
        await WaitUntilAsync(() =>
        {
            targetReady.Reset();
            target.DrainReady(
                MeshReadyDomains.Application, targetReady, RecvFlags.DontWait);
            requestReadyIndex = Enumerable.Range(0, targetReady.Count)
                .FirstOrDefault(
                    index => actorRequest
                        ? targetReady[index].OwnerKind == MeshOwnerKind.Actor
                        : targetReady[index].OwnerKind == MeshOwnerKind.Spot
                          && targetReady[index].SpotId == spot.SpotId,
                    -1);
            return requestReadyIndex >= 0;
        });
        using var targetClaim = targetReady.TakeClaim(requestReadyIndex);
        using var targetBatch = new MeshReceiveBatch();
        Assert.True(targetClaim.Receive(targetBatch, RecvFlags.DontWait));
        var request = Assert.Single(
            Enumerable.Range(0, targetBatch.Count)
                .Select(index => targetBatch[index]));
        Assert.Equal(
            actorRequest ? MeshRecordKind.ActorRequest : MeshRecordKind.SpotRequest,
            request.Kind);
        Assert.Equal(operationId, request.OperationId);
        Assert.Equal(operationId.Low, request.ReplyRouteId);

        using var replyPart = Message.From(new byte[] { 9, 2, 6, 5 });
        Assert.Equal(SubmitResult.Ok, request.Reply([replyPart]));
        // A native request window accepts one terminal reply only.
        Assert.Equal(SubmitResult.InvalidState, request.Reply([replyPart]));

        using var sourceReady = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            sourceReady.Reset();
            source.DrainReady(
                MeshReadyDomains.Infrastructure, sourceReady, RecvFlags.DontWait);
            return sourceReady.Count == 1;
        });
        using var sourceClaim = sourceReady.TakeClaim(0);
        using var sourceBatch = new MeshReceiveBatch();
        Assert.True(sourceClaim.Receive(sourceBatch, RecvFlags.DontWait));
        var completion = Assert.Single(
            Enumerable.Range(0, sourceBatch.Count)
                .Select(index => sourceBatch[index]));
        Assert.Equal(MeshRecordKind.Completion, completion.Kind);
        Assert.Equal(operationId, completion.OperationId);
        Assert.Equal(
            actorRequest ? MeshOperationKind.ActorRequest : MeshOperationKind.SpotRequest,
            completion.OperationKind);
        Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
        Assert.Equal(0, completion.FailureErrno);
        var retainedReply = sourceBatch.RetainMessage(0);
        try
        {
            Assert.Equal(
                new byte[] { 9, 2, 6, 5 },
                Assert.Single(retainedReply).ToArray());
        }
        finally
        {
            foreach (var part in retainedReply)
                part.Dispose();
        }
        sourceBatch.Reset();
        Assert.False(sourceClaim.Receive(sourceBatch, RecvFlags.DontWait));
    }

    [Fact]
    public async Task MessageFollowAsyncAdmission_ReleasesCoreApplicationLeases()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "message-follow");
        await using var target = new ZLinkManagedMeshNode(context, "message-follow");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"message-follow-source-{suffix}");
        var targetRid = RoutingId.From($"message-follow-target-{suffix}");
        var sourceEndpoint = $"inproc://message-follow-source-{suffix}";
        var targetEndpoint = $"inproc://message-follow-target-{suffix}";
        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        source.SetLocalOwnerLeaseGeneration(17);
        target.SetLocalOwnerLeaseGeneration(17);
        var sourceSpot = (ZLinkManagedSpot)source.EntrySpot();
        var targetSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            $"message-follow-spot-{suffix}",
            out _);
        target.Start();
        source.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);
        context.ResetCoreHwmBudgetMetrics();

        using var first = Message.From("first");
        using var second = Message.From("second");
        var operationId = new MeshOperationId(41, 146);
        await source.MessageFollowSendToSpotAsync(
            sourceSpot.SpotId,
            targetRid,
            targetSpot.SpotId,
            targetSpot.LifecycleGeneration,
            operationId,
            target.Status().LifecycleGeneration,
            targetSpot.AuthorityOwnerGeneration,
            ownerLeaseGeneration: 17,
            messageFollowHopCount: 1,
            [first, second],
            metadata: default,
            CancellationToken.None);

        using var ready = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            ready.Reset();
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count == 1;
        });
        using (var claim = ready.TakeClaim(0))
        using (var received = new MeshReceiveBatch())
        {
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            var record = Assert.Single(
                Enumerable.Range(0, received.Count)
                    .Select(index => received[index]));
            Assert.Equal(MeshRecordKind.SpotSend, record.Kind);
            Assert.Equal(operationId, record.OperationId);
            Assert.Equal(2, record.PartCount);
            var retained = received.RetainMessage(0);
            try
            {
                Assert.Equal("first", retained[0].GetString());
                Assert.Equal("second", retained[1].GetString());
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(retained);
            }
            var held = context.GetCoreHwmBudgetSnapshot();
            Assert.True(held.OutstandingApplicationLeaseCount > 0);
            Assert.True(held.ApplicationAccountedBytes > 0);
        }

        await WaitUntilAsync(() =>
        {
            var snapshot = context.GetCoreHwmBudgetSnapshot();
            return snapshot.OutstandingApplicationLeaseCount == 0
                   && snapshot.ApplicationAccountedBytes == 0;
        });
        var released = context.GetCoreHwmBudgetSnapshot();
        Assert.Equal(0UL, released.OutstandingApplicationLeaseCount);
        Assert.Equal(0UL, released.ApplicationAccountedBytes);
    }

    [Fact]
    public async Task ManagedNodes_AdmitAndRouteChannelOverRawRouter()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "orders");
        await using var target = new ZLinkManagedMeshNode(context, "orders");
        var sourceRid = RoutingId.From("orders-source");
        var targetRid = RoutingId.From("orders-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://orders-source-{suffix}";
        var targetEndpoint = $"inproc://orders-target-{suffix}";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.AddChannel("worker");
        target.Start();
        source.Start();

        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        using var payload = Message.From(new byte[] { 7, 8, 9 });
        Assert.Equal(
            SubmitResult.Ok,
            source.EntrySpot().SendToChannel("worker", [payload]));

        using var ready = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            ready.Reset();
            target.DrainReady(
                MeshReadyDomains.All,
                ready,
                RecvFlags.DontWait);
            return ready.Count == 1;
        });

        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.ChannelSend, received[0].Kind);
        Assert.Equal("worker", received[0].ChannelName);
        Assert.Equal(sourceRid, received[0].SourceNodeRid);
    }

    [Fact]
    public async Task RawRouterPort_EnforcesLifecycleAndPreservesMultipartOwnership()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = $"inproc://raw-port-{suffix}";
        var senderRid = RoutingId.From("raw-sender");
        var receiverRid = RoutingId.From("raw-receiver");
        using var sender = new ZLinkRawRouterServicePort(
            context,
            senderRid,
            endpoint + "-sender");
        using var receiver = new ZLinkRawRouterServicePort(
            context,
            receiverRid,
            endpoint);

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            sender.SendAsync(
                receiverRid,
                new ReadOnlyMemory<byte>[] { new byte[] { 1 } }));

        receiver.Start();
        sender.Start();
        sender.Connect(endpoint, receiverRid);

        var payload = new ReadOnlyMemory<byte>[]
        {
            new byte[] { 1, 2, 3 },
            new byte[] { 4, 5 }
        };
        await sender.SendAsync(receiverRid, payload);

        ZLinkRawRouterEnvelope? envelope = null;
        Assert.True(SpinWait.SpinUntil(
            () => receiver.TryReceive(out envelope),
            TimeSpan.FromSeconds(5)));
        var receivedEnvelope = Assert.IsType<ZLinkRawRouterEnvelope>(envelope);
        using (receivedEnvelope)
        {
            Assert.Equal(senderRid, receivedEnvelope.SourceRoutingId);
            Assert.Equal(2, receivedEnvelope.Parts.Count);
            Assert.Equal(
                new byte[] { 1, 2, 3 },
                receivedEnvelope.Parts[0].ToArray());
            Assert.Equal(
                new byte[] { 4, 5 },
                receivedEnvelope.Parts[1].ToArray());
        }

        var requestTask = sender.RequestAsync(
            receiverRid,
            new ReadOnlyMemory<byte>[] { new byte[] { 9, 8 } },
            TimeSpan.FromSeconds(3));
        envelope = null;
        Assert.True(SpinWait.SpinUntil(
            () => receiver.TryReceive(out envelope),
            TimeSpan.FromSeconds(5)));
        var requestEnvelope = Assert.IsType<ZLinkRawRouterEnvelope>(envelope);
        using (requestEnvelope)
        {
            Assert.True(requestEnvelope.CanReply);
            requestEnvelope.Reply(
                new ReadOnlyMemory<byte>[] { new byte[] { 7, 6, 5 } });
        }
        using var reply = await requestTask;
        Assert.Equal(new byte[] { 7, 6, 5 },
            Assert.Single(reply.Parts).ToArray());

        sender.Dispose();
        Assert.Throws<ObjectDisposedException>(() => sender.TryReceive(out _));
    }

    [Fact]
    public async Task Manual_Object_Client_Pair_Without_Server_Channel_Ends_As_NotRequired()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(context, "orders");
        await using var right = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var leftEndpoint = $"inproc://orders-client-left-{suffix}";
        var rightEndpoint = $"inproc://orders-client-right-{suffix}";

        left.SetRoutingId(RoutingId.From("orders-client-left"));
        left.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        left.SetBind(leftEndpoint);
        left.ConnectPeer(rightEndpoint, RoutingId.From("orders-client-right"));
        right.SetRoutingId(RoutingId.From("orders-client-right"));
        right.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        right.SetBind(rightEndpoint);
        // One manual connection is sufficient to classify both object-client
        // peers; symmetric intents would create two independent connection
        // attempts before the NotRequired state is published.
        await using var leftMonitor = left.OpenMonitor();
        await using var rightMonitor = right.OpenMonitor();
        right.Start();
        left.Start();

        await WaitUntilAsync(() =>
            left.Peers().Length == 1
            && right.Peers().Length == 1
            && left.Peers().All(static peer =>
                peer.State == MeshPeerState.NotRequired)
            && right.Peers().All(static peer =>
                peer.State == MeshPeerState.NotRequired));

        Assert.Equal(0u, left.Status().AdmittedPeerCount);
        Assert.Equal(0u, right.Status().AdmittedPeerCount);
        Assert.Single(left.Peers());
        Assert.Single(right.Peers());
        Assert.All(
            left.Peers().Concat(right.Peers()),
            static peer => Assert.Equal(MeshPeerState.NotRequired, peer.State));
        Assert.Equal(0UL, leftMonitor.Status().PeerRejected);
        Assert.Equal(0UL, rightMonitor.Status().PeerRejected);
        var observedNotRequired = false;
        await WaitUntilAsync(() =>
        {
            observedNotRequired |= Drain(leftMonitor)
                .Concat(Drain(rightMonitor))
                .Any(static meshEvent =>
                    meshEvent.Kind == MeshMonitorEventKind.PeerNotRequired);
            return observedNotRequired;
        });
        Assert.True(observedNotRequired);
    }

    [Fact]
    public async Task Manual_Object_Client_Pair_With_Zero_Weight_Server_Channel_Is_Admitted()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(context, "orders");
        await using var right = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var leftEndpoint = $"inproc://orders-channel-left-{suffix}";
        var rightEndpoint = $"inproc://orders-channel-right-{suffix}";

        left.SetRoutingId(RoutingId.From("orders-channel-left"));
        left.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        left.AddChannel("orders");
        left.SetChannelWeight("orders", 0);
        left.SetBind(leftEndpoint);
        left.ConnectPeer(rightEndpoint, RoutingId.From("orders-channel-right"));
        right.SetRoutingId(RoutingId.From("orders-channel-right"));
        right.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        right.SetBind(rightEndpoint);
        right.Start();
        left.Start();

        await WaitUntilAsync(() =>
            left.Peers().Any(static peer => peer.State == MeshPeerState.Admitted)
            && right.Peers().Any(static peer => peer.State == MeshPeerState.Admitted));

        Assert.Equal(1u, left.Status().AdmittedPeerCount);
        Assert.Equal(1u, right.Status().AdmittedPeerCount);
        Assert.DoesNotContain(
            left.Peers().Concat(right.Peers()),
            static peer => peer.State == MeshPeerState.NotRequired);
    }

    [Fact]
    public async Task Repeated_Client_Hello_Without_Reading_Admit_Keeps_One_NotRequired_Peer()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var target = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var targetEndpoint = $"inproc://orders-client-target-{suffix}";
        var sourceEndpoint = $"inproc://orders-client-source-{suffix}";
        var sourceRid = RoutingId.From("orders-client-source");

        target.SetRoutingId(RoutingId.From("orders-client-target"));
        target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        target.SetBind(targetEndpoint);
        target.Start();

        using var source = context.CreateDealerSocket();
        source.SetRoutingId(sourceRid);
        source.Connect(targetEndpoint);
        var encodedHello = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            "orders",
            sourceEndpoint,
            lifecycleGeneration: 1,
            descriptorRevision: 1,
            new Dictionary<string, uint>(StringComparer.Ordinal),
            objectRole: (byte)ZLinkMeshNodeObjectRole.Client);
        using (var firstHello = Message.From(encodedHello))
            await source.Send()
                .Message(firstHello)
                .Async(CancellationToken.None);
        using (var repeatedHello = Message.From(encodedHello))
            await source.Send()
                .Message(repeatedHello)
                .Async(CancellationToken.None);

        await WaitUntilAsync(() =>
            target.Peers().Length == 1
            && target.Peers()[0].State == MeshPeerState.NotRequired);
        await Task.Delay(TimeSpan.FromMilliseconds(1100));

        var peer = Assert.Single(target.Peers());
        Assert.Equal(sourceRid, peer.RoutingId);
        Assert.Equal(MeshPeerState.NotRequired, peer.State);
    }

    [Fact]
    public async Task Framework_Node_Relay_To_ObjectClient_Uses_The_Admitted_Connection()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var server = new ZLinkManagedMeshNode(context, "orders");
        await using var client = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var serverEndpoint = $"inproc://orders-server-{suffix}";
        var clientEndpoint = $"inproc://orders-client-{suffix}";
        var clientRid = RoutingId.From("orders-client");

        server.SetRoutingId(RoutingId.From("orders-server"));
        server.SetObjectRole(ZLinkMeshNodeObjectRole.Server);
        server.SetBind(serverEndpoint);
        server.ConnectPeer(clientEndpoint, clientRid);
        client.SetRoutingId(clientRid);
        client.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        client.SetBind(clientEndpoint);
        client.Start();
        server.Start();

        await WaitUntilAsync(() =>
            server.Peers().Any(static peer => peer.State == MeshPeerState.Admitted)
            && client.Peers().Any(static peer => peer.State == MeshPeerState.Admitted));
        var peerCount = server.Peers().Length;
        using var payload = Message.From(new byte[] { 1 });

        Assert.Equal(SubmitResult.Ok, server.SendToNode(clientRid, [payload]));
        Assert.Equal(peerCount, server.Peers().Length);

        using var ready = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            ready.Reset();
            client.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count > 0;
        });
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Contains(
            MeshRecordKind.NodeSend,
            Enumerable.Range(0, received.Count)
                .Select(index => received[index].Kind));
    }

    [Fact]
    public async Task Managed_mesh_node_waits_for_shared_application_permit_before_core_receive()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var applicationJobQueue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        await using var source = new ZLinkManagedMeshNode(context, "orders");
        await using var target = new ZLinkManagedMeshNode(
            context,
            "orders",
            applicationJobQueue: applicationJobQueue);
        await using var pump = new ZLinkMeshDispatchPump(
            target,
            new ZLinkMeshCompletionTable());
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://orders-pre-receive-source-{suffix}";
        var targetEndpoint = $"inproc://orders-pre-receive-target-{suffix}";
        var sourceRid = RoutingId.From("orders-pre-receive-source");
        var targetRid = RoutingId.From("orders-pre-receive-target");

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.Start();
        pump.EnsureStarted();
        source.Start();

        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        using var occupied = await applicationJobQueue
            .AcquireAsync(CancellationToken.None);
        using var payload = Message.From(new byte[] { 1, 2, 3 });
        Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [payload]));

        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().CapacityWaiters == 1);
        Assert.Equal(0UL, target.Status().PendingApplicationMessages);
    }

    [Fact]
    public async Task Managed_mesh_node_releases_shared_permit_after_finite_control_and_malformed_records()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var applicationJobQueue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        await using var target = new ZLinkManagedMeshNode(
            context,
            "orders",
            applicationJobQueue: applicationJobQueue);
        await using var monitor = target.OpenMonitor();
        var suffix = Guid.NewGuid().ToString("N");
        var targetEndpoint = $"inproc://orders-finite-control-target-{suffix}";
        var sourceEndpoint = $"inproc://orders-finite-control-source-{suffix}";
        var sourceRid = RoutingId.From("orders-finite-control-source");

        target.SetRoutingId(RoutingId.From("orders-finite-control-target"));
        target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        target.SetBind(targetEndpoint);
        target.Start();

        using var source = context.CreateDealerSocket();
        source.SetRoutingId(sourceRid);
        source.Connect(targetEndpoint);
        using (var hello = Message.From(
                   ZLinkServiceWireCodec.EncodeRouteAdmission(
                       ServiceWireConstants.Command.Hello,
                       "orders",
                       sourceEndpoint,
                       lifecycleGeneration: 1,
                       descriptorRevision: 1,
                       new Dictionary<string, uint>(StringComparer.Ordinal),
                       objectRole: (byte)ZLinkMeshNodeObjectRole.Client)))
            await source.Send()
                .Message(hello)
                .Async(CancellationToken.None);

        await WaitUntilAsync(() => target.Peers().Length == 1);
        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().PermitsInUse == 0);
        var controlStatus = applicationJobQueue.GetStatus();
        Assert.Equal(1UL, controlStatus.PeakPermitsInUse);
        Assert.Equal(0UL, controlStatus.ReservedSupplyPermits);
        Assert.Equal(0UL, controlStatus.QueuedApplicationJobs);

        applicationJobQueue.ResetMetrics();
        var protocolErrors = monitor.Status().ProtocolErrors;
        using (var malformed = Message.From(new byte[] { 1, 2, 3, 4, 5 }))
            await source.Send()
                .Message(malformed)
                .Async(CancellationToken.None);

        await WaitUntilAsync(() => monitor.Status().ProtocolErrors > protocolErrors);
        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().PermitsInUse == 0);
        var malformedStatus = applicationJobQueue.GetStatus();
        Assert.Equal(1UL, malformedStatus.PeakPermitsInUse);
        Assert.Equal(0UL, malformedStatus.ReservedSupplyPermits);
        Assert.Equal(0UL, malformedStatus.QueuedApplicationJobs);
    }

    [Fact]
    public async Task Managed_mesh_logical_multicast_reacquires_one_shared_permit_per_child()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var applicationJobQueue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        await using var source = new ZLinkManagedMeshNode(context, "events");
        await using var target = new ZLinkManagedMeshNode(
            context,
            "events",
            applicationJobQueue: applicationJobQueue);
        await using var pump = new ZLinkMeshDispatchPump(
            target,
            new ZLinkMeshCompletionTable(),
            applicationJobQueue);
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://events-multicast-source-{suffix}";
        var targetEndpoint = $"inproc://events-multicast-target-{suffix}";
        var sourceRid = RoutingId.From("events-multicast-source");
        var targetRid = RoutingId.From("events-multicast-target");
        const string channelName = "events";
        const string topic = "created";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.AddChannel(channelName);
        var firstSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            $"events-first-{suffix}",
            out _);
        var secondSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            $"events-second-{suffix}",
            out _);
        firstSpot.SetSubscription(channelName, topic);
        secondSpot.SetSubscription(channelName, topic);

        var firstDelivery = new TaskCompletionSource<ZLinkBackendSubscribeMessage>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondDelivery = new TaskCompletionSource<ZLinkBackendSubscribeMessage>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var deliveries = 0;
        void Capture(ZLinkMeshDispatchPump.SpotDispatchState state)
        {
            if (!state.Subscriptions.TryDequeue(out var message))
                return;
            if (Interlocked.Increment(ref deliveries) == 1)
                firstDelivery.TrySetResult(message);
            else
                secondDelivery.TrySetResult(message);
        }

        var firstState = pump.RegisterSpot(firstSpot.SpotId);
        var secondState = pump.RegisterSpot(secondSpot.SpotId);
        pump.SetDispatchHandler(firstSpot.SpotId, _ => Capture(firstState));
        pump.SetDispatchHandler(secondSpot.SpotId, _ => Capture(secondState));

        target.Start();
        pump.EnsureStarted();
        source.Start();
        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);

        var sourceSpot = source.EntrySpot();
        using var payload = Message.From(new byte[] { 7, 8, 9 });
        sourceSpot.Publish(channelName, topic, [payload]);

        using var first = await firstDelivery.Task
            .WaitAsync(TimeSpan.FromSeconds(3));
        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().CapacityWaiters == 1);
        Assert.False(secondDelivery.Task.IsCompleted);
        var firstStatus = applicationJobQueue.GetStatus();
        Assert.Equal(1UL, firstStatus.QueuedApplicationJobs);
        Assert.Equal(1UL, firstStatus.PermitsInUse);

        Assert.NotNull(first.ApplicationJobAdmission);
        first.ApplicationJobAdmission!.ReleaseForHandlerStart();

        using var second = await secondDelivery.Task
            .WaitAsync(TimeSpan.FromSeconds(3));
        var secondStatus = applicationJobQueue.GetStatus();
        Assert.Equal(1UL, secondStatus.QueuedApplicationJobs);
        Assert.Equal(1UL, secondStatus.PermitsInUse);
        Assert.NotNull(second.ApplicationJobAdmission);
        second.ApplicationJobAdmission!.ReleaseForHandlerStart();

        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().PermitsInUse == 0);
        Assert.Equal(2, Volatile.Read(ref deliveries));
    }

    [Fact]
    public async Task Managed_mesh_shutdown_keeps_shared_core_credit_until_the_admitted_multicast_child_finishes()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var applicationJobQueue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        await using var source = new ZLinkManagedMeshNode(context, "events");
        await using var target = new ZLinkManagedMeshNode(
            context,
            "events",
            applicationJobQueue: applicationJobQueue);
        await using var pump = new ZLinkMeshDispatchPump(
            target,
            new ZLinkMeshCompletionTable(),
            applicationJobQueue);
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://events-shutdown-source-{suffix}";
        var targetEndpoint = $"inproc://events-shutdown-target-{suffix}";
        var sourceRid = RoutingId.From("events-shutdown-source");
        var targetRid = RoutingId.From("events-shutdown-target");
        const string channelName = "events";
        const string topic = "created";

        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.AddChannel(channelName);
        var firstSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            $"events-shutdown-first-{suffix}",
            out _);
        var secondSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            $"events-shutdown-second-{suffix}",
            out _);
        firstSpot.SetSubscription(channelName, topic);
        secondSpot.SetSubscription(channelName, topic);

        var firstDelivery = new TaskCompletionSource<ZLinkBackendSubscribeMessage>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        void Capture(ZLinkMeshDispatchPump.SpotDispatchState state)
        {
            if (state.Subscriptions.TryDequeue(out var message))
                firstDelivery.TrySetResult(message);
        }
        var firstState = pump.RegisterSpot(firstSpot.SpotId);
        var secondState = pump.RegisterSpot(secondSpot.SpotId);
        pump.SetDispatchHandler(firstSpot.SpotId, _ => Capture(firstState));
        pump.SetDispatchHandler(secondSpot.SpotId, _ => Capture(secondState));

        target.Start();
        pump.EnsureStarted();
        source.Start();
        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);
        context.ResetCoreHwmBudgetMetrics();

        var sourceSpot = source.EntrySpot();
        using var payload = Message.From(new byte[] { 7, 8, 9 });
        sourceSpot.Publish(channelName, topic, [payload]);

        using var first = await firstDelivery.Task
            .WaitAsync(TimeSpan.FromSeconds(3));
        await WaitUntilAsync(() =>
            applicationJobQueue.GetStatus().CapacityWaiters == 1);
        Assert.True(
            context.GetCoreHwmBudgetSnapshot().OutstandingApplicationLeaseCount > 0);

        await target.DisposeAsync();

        Assert.True(
            context.GetCoreHwmBudgetSnapshot().OutstandingApplicationLeaseCount > 0);
        first.ApplicationJobAdmission?.ReleaseForHandlerStart();
        first.Dispose();
        await WaitUntilAsync(() =>
            context.GetCoreHwmBudgetSnapshot().OutstandingApplicationLeaseCount == 0);
    }

    [Fact]
    public async Task UnknownRidAdmissionsRemainBoundToTheirConfiguredEndpoints()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "orders");
        await using var targetA = new ZLinkManagedMeshNode(context, "orders");
        await using var targetB = new ZLinkManagedMeshNode(context, "orders");
        var suffix = Guid.NewGuid().ToString("N");
        var endpointA = $"inproc://orders-a-{suffix}";
        var endpointB = $"inproc://orders-b-{suffix}";

        source.SetRoutingId(RoutingId.From("orders-source"));
        source.SetBind($"inproc://orders-source-{suffix}");
        source.ConnectPeer(endpointA);
        source.ConnectPeer(endpointB);
        targetA.SetRoutingId(RoutingId.From("orders-a"));
        targetA.SetBind(endpointA);
        targetA.AddChannel("worker");
        targetB.SetRoutingId(RoutingId.From("orders-b"));
        targetB.SetBind(endpointB);
        targetB.AddChannel("worker");

        targetA.Start();
        targetB.Start();
        source.Start();

        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 2);
        var admittedEndpoints = source.Peers()
            .Where(static peer => peer.State == MeshPeerState.Admitted)
            .Select(static peer => peer.Endpoint)
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            new[] { endpointA, endpointB }.SequenceEqual(admittedEndpoints));

        for (var index = 0; index < 4; index++)
        {
            using var payload = Message.From(new byte[] { checked((byte)index) });
            Assert.Equal(
                SubmitResult.Ok,
                source.EntrySpot().SendToChannel("worker", [payload]));
        }

        await WaitForApplicationRecordAsync(targetA);
        await WaitForApplicationRecordAsync(targetB);
    }

    [Fact]
    public async Task CompletionTable_CompletesTaskExactlyOnce()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(0, 7);
        var calls = 0;
        Assert.True(table.Register(operation, (_, _) => Interlocked.Increment(ref calls)));

        var record = Completion(operation);
        table.Complete(record, Array.Empty<Message>());
        table.Complete(record, Array.Empty<Message>());
        await table.CompletionDrained;

        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task CompletionTable_FailAllPreservesTerminalResult()
    {
        var table = new ZLinkMeshCompletionTable();
        var operation = new MeshOperationId(0, 8);
        RequestResult? completed = null;
        Assert.True(table.RegisterRequest(
            operation,
            (result, _) => completed = result));

        table.FailAll(RequestResult.Terminated);
        await table.CompletionDrained;

        Assert.Equal(RequestResult.Terminated, completed);
    }

    [Fact]
    public async Task CompletionTable_RegisterBeforeSubmitDispatchesOnSharedLane()
    {
        var table = new ZLinkMeshCompletionTable();
        var correlation = new MeshOperationId(7, 10);
        var completedDuringSubmit = false;

        var submit = table.RegisterRequestBeforeSubmit(
            correlation,
            (result, reply) =>
            {
                Assert.Equal(RequestResult.Ok, result);
                Assert.Empty(reply);
                Assert.True(ZLinkCompletionDispatcher.IsCurrentExecution);
                completedDuringSubmit = true;
            },
            id =>
            {
                Assert.Equal(correlation, id);
                table.Complete(Completion(id), Array.Empty<Message>());
                return SubmitResult.Ok;
            });

        Assert.Equal(SubmitResult.Ok, submit);
        await table.CompletionDrained;
        Assert.True(completedDuringSubmit);
    }

    [Fact]
    public void CompletionTable_SubmitRejectionRemovesWaiter()
    {
        var table = new ZLinkMeshCompletionTable();
        var correlation = new MeshOperationId(7, 11);
        var calls = 0;
        Assert.Equal(
            SubmitResult.Backpressured,
            table.RegisterBeforeSubmit(
                correlation,
                (_, _) => calls++,
                _ => SubmitResult.Backpressured));

        using var late = Message.From(new byte[] { 1 });
        table.Complete(Completion(correlation), [late]);

        Assert.Equal(0, calls);
        Assert.Throws<ObjectDisposedException>(() => _ = late.Size);
    }

    [Fact]
    public async Task InstanceAuthority_UsesExactStoreVersionAcrossReadyAndRelease()
    {
        var store = new ZLinkInMemoryLocationStore();
        var ownerRid = RoutingId.From("owner");
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "owner",
                TimeSpan.FromMinutes(1))).Token;
        var descriptor = new ZLinkMeshNodeDescriptor(
            "mesh",
            ownerRid,
            3,
            1,
            "inproc://owner",
            new Dictionary<string, int> { ["mesh"] = 100 },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            DateTimeOffset.UtcNow)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "entry-owner",
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    "cart",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 0),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        "cart",
                        0,
                        0,
                        0)
                ])
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot");
        var creating = new ZLinkInstanceSpotAuthorityPayload(
            ZLinkInstanceSpotAuthorityState.Creating,
            "spot",
            "cart",
            "mesh",
            ownerRid,
            3,
            owner.OwnerId,
            checked((ulong)owner.LeaseGeneration),
            null,
            0,
            0);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    key,
                    "cart",
                    "inline-v1:00000000:",
                    new byte[32],
                    0,
                    new ZLinkMeshNodeDescriptorKey("mesh", ownerRid),
                    3,
                    owner,
                    ZLinkInstanceSpotAuthorityPayloadCodec.Encode(creating),
                    new ZLinkCapacityVector(
                        0,
                        1,
                        new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.InstanceSpot,
                            "cart",
                            1)))));
        var readyPayload = creating with
        {
            State = ZLinkInstanceSpotAuthorityState.Ready
        };
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reserved.Reservation,
                ZLinkInstanceSpotAuthorityPayloadCodec.Encode(readyPayload)));
        Assert.Equal(
            reserved.Reservation.ObjectGeneration,
            committed.Snapshot.ObjectGeneration);
        Assert.Equal(
            reserved.Reservation.AuthorityOwnerGeneration,
            committed.Snapshot.AuthorityOwnerGeneration);

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                key,
                reserved.Reservation.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(key));
    }

    [Fact]
    public async Task MonitorClose_RejectsLateResourcePublication()
    {
        var monitor = new RawMeshMonitor();
        await monitor.DisposeAsync();
        monitor.Publish(MeshMonitorEventKind.StateChanged, MeshNodeState.Stopped);
        Assert.Null(monitor.Recv(RecvFlags.DontWait));
        Assert.Equal(MeshNodeState.Stopped, monitor.Status().State);
    }

    private static IReadOnlyList<MeshMonitorEvent> Drain(
        IMeshNodeMonitor monitor)
    {
        var events = new List<MeshMonitorEvent>();
        while (monitor.Recv(RecvFlags.DontWait) is { } meshEvent)
            events.Add(meshEvent);
        return events;
    }

    private static MeshReceiveRecord Completion(MeshOperationId operation) =>
        new(
            MeshRecordKind.Completion,
            MeshReadyDomains.Infrastructure,
            default,
            string.Empty,
            0,
            default,
            operation,
            MeshOperationKind.NodeRequest,
            null,
            null,
            null,
            0,
            0,
            0,
            0,
            null);

    private static ZLinkServiceWireCodec.DecodeError ExpectedError(string error) =>
        error switch
        {
            "invalid-magic" => ZLinkServiceWireCodec.DecodeError.InvalidMagic,
            "unknown-command" => ZLinkServiceWireCodec.DecodeError.UnknownCommand,
            "forbidden-flag" => ZLinkServiceWireCodec.DecodeError.ForbiddenFlag,
            "invalid-field" => ZLinkServiceWireCodec.DecodeError.InvalidField,
            "truncated-field" => ZLinkServiceWireCodec.DecodeError.TruncatedField,
            "trailing-byte" => ZLinkServiceWireCodec.DecodeError.TrailingByte,
            _ => throw new InvalidOperationException(error)
        };

    private static ZLinkServiceWireCodec.AdmissionRecord DecodeAdmission(byte[] bytes)
    {
        Assert.True(ZLinkServiceWireCodec.TryDecodeRouteAdmission(
            bytes,
            out _,
            out var admission,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        return admission;
    }

    private static byte[] AppendDescriptorExtension(
        byte[] encoded,
        byte id,
        ReadOnlySpan<byte> value)
    {
        var offset = 10;
        offset += 1 + encoded[offset];
        offset += 1 + encoded[offset];
        offset += sizeof(ulong) + sizeof(ulong);
        var endpointLength = BinaryPrimitives.ReadUInt16BigEndian(encoded.AsSpan(offset));
        offset += sizeof(ushort) + endpointLength;
        var channelCount = BinaryPrimitives.ReadUInt16BigEndian(encoded.AsSpan(offset));
        offset += sizeof(ushort);
        for (var index = 0; index < channelCount; index++)
        {
            offset += 1 + encoded[offset];
            offset += sizeof(uint);
        }

        var previousLength = BinaryPrimitives.ReadUInt32BigEndian(encoded.AsSpan(offset));
        var result = new byte[encoded.Length + 1 + sizeof(uint) + value.Length];
        encoded.CopyTo(result, 0);
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(offset),
            checked(previousLength + (uint)(1 + sizeof(uint) + value.Length)));
        var tail = encoded.Length;
        result[tail] = id;
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(tail + 1),
            checked((uint)value.Length));
        value.CopyTo(result.AsSpan(tail + 1 + sizeof(uint)));
        BinaryPrimitives.WriteUInt32BigEndian(
            result.AsSpan(6),
            checked((uint)(result.Length - 10)));
        return result;
    }

    private static int FindSequence(byte[] bytes, ReadOnlySpan<byte> sequence)
    {
        for (var offset = 0; offset <= bytes.Length - sequence.Length; offset++)
            if (bytes.AsSpan(offset, sequence.Length).SequenceEqual(sequence))
                return offset;
        throw new InvalidOperationException("Test sequence was not found.");
    }

    private static async Task WaitForApplicationRecordAsync(
        ZLinkManagedMeshNode node)
    {
        using var ready = new MeshReadyBatch();
        await WaitUntilAsync(() =>
        {
            ready.Reset();
            node.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count > 0;
        });
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Contains(
            MeshRecordKind.ChannelSend,
            Enumerable.Range(0, received.Count)
                .Select(index => received[index].Kind));
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        while (!condition())
        {
            if (Stopwatch.GetTimestamp() >= deadline)
                throw new TimeoutException("The managed MeshNode condition was not reached.");
            await Task.Delay(10);
        }
    }
}
