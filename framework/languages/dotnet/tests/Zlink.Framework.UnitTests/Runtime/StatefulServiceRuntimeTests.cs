using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.UnitTests;

public sealed class StatefulServiceRuntimeTests
{
    [Fact]
    public void ActorCreationWireAndDurableTerminalPreserveIdentityAndRejectedReply()
    {
        var target = RoutingId.From("actor-target");
        var actor = new ActorRef("actor-1", 17, "play", target);
        var completion = new ActorCreateCompletion(ActorCreateResult.Created, actor);
        var replyFrame = ZLinkServiceWireCodec.EncodeActorCreateReply(
            31,
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            completion);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            replyFrame,
            out var reply,
            out _));
        Assert.True(ZLinkServiceWireCodec.TryDecodeActorCreateReply(
            reply,
            "play",
            out var decoded,
            out _));
        Assert.Equal(actor, decoded!.Actor);

        var authority = new ZLinkActorAuthorityPayload(
            ZLinkActorAuthorityState.Creating,
            "player",
            "actor-1",
            "entry-1",
            13,
            ZLinkSpotKind.Entry,
            "owner-1",
            23,
            "mesh-a",
            target,
            29);
        var encodedAuthority = ZLinkActorAuthorityPayloadCodec.Encode(authority);
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(
            encodedAuthority,
            out var decodedAuthority));
        Assert.Equal(authority, decodedAuthority);

        var codecs = new ZLinkCodecRegistryBuilder();
        var applicationReply = ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Response,
                "actor.create.reply",
                string.Empty,
                ZLinkEnvelopeCodec.DefaultContentType,
                "31",
                null, null, null, null),
            ZLinkMessage.From(new byte[] { 7, 8, 9 }),
            typeof(ZLinkMessage),
            codecs);
        var terminal = new ActorCreateOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new ActorCreateCompletion(ActorCreateResult.Rejected, default),
            applicationReply.Select(static part =>
                (ReadOnlyMemory<byte>)part.AsReadOnlyMemory().ToArray()).ToArray());
        try
        {
            var encodedTerminal = ZLinkActorCreationTerminalCodec.Encode(terminal, codecs);
            Assert.True(ZLinkActorCreationTerminalCodec.TryDecode(
                encodedTerminal,
                codecs,
                out var decodedTerminal));
            Assert.Equal(ActorCreateResult.Rejected, decodedTerminal.Completion!.Result);
            Assert.Equal(2, decodedTerminal.ReplyParts!.Count);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(applicationReply);
        }
    }

    [Fact]
    public void RelocationApplicationPayloadEnvelopePreservesContentTypeAndPayload()
    {
        var encoded = ZLinkApplicationPayloadEnvelopeCodec.Encode(
            "create",
            "application/x-zlink-test",
            [1, 2, 3, 4]);

        Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecode(
            encoded,
            out var decoded));
        Assert.Equal("create", decoded.PacketName);
        Assert.Equal("application/x-zlink-test", decoded.ContentType);
        Assert.Equal([1, 2, 3, 4], decoded.Payload.ToArray());

        var reference = ZLinkInlineCreationIntentCodec.Encode(encoded);
        Assert.StartsWith("inline-v1:", reference, StringComparison.Ordinal);
        Assert.True(ZLinkInlineCreationIntentCodec.TryDecode(
            reference,
            out var restored));
        Assert.Equal(encoded, restored);
        Assert.False(ZLinkInlineCreationIntentCodec.TryDecode(
            reference[..^1] + (reference[^1] == 'A' ? "B" : "A"),
            out _));
    }

    [Fact]
    public void FrameworkMultipartGoldenFixtureMatchesTheManagedCodec()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/framework-multipart-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));

        var valid = document.RootElement.GetProperty("valid")[0];
        var encoded = Convert.FromHexString(valid.GetProperty("encodedHex").GetString()!);
        Assert.True(ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
            encoded,
            out var parts));
        try
        {
            var expected = valid.GetProperty("partsHex").EnumerateArray()
                .Select(static item => Convert.FromHexString(item.GetString()!))
                .ToArray();
            Assert.Equal(expected.Length, parts.Length);
            for (var index = 0; index < parts.Length; index++)
                Assert.Equal(expected[index], parts[index].AsReadOnlyMemory().ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        foreach (var invalid in document.RootElement.GetProperty("invalid").EnumerateArray())
        {
            var bytes = Convert.FromHexString(invalid.GetProperty("encodedHex").GetString()!);
            Assert.False(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                    bytes,
                    out var rejected),
                invalid.GetProperty("name").GetString());
            ZLinkMessageParts.DisposeAll(rejected);
        }
    }

    [Fact]
    public void InstanceSpotColdActivationWirePreservesDescriptorPlacementAndDeadline()
    {
        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                "target-mesh",
                RoutingId.From("target-node"),
                17,
                "instance-spot",
                "Sample.InstanceSpot",
                "descriptor-23"),
            RoutingId.From("source-node"),
            29,
            "source-spot",
            new MeshOperationId(31, 37),
            IsRequest: true,
            ReplyRouteId: 41,
            DeadlineUnixMs: 4_102_444_800_000);

        var encoded = ZLinkServiceWireCodec.EncodeInstanceSpotActivation(
            operation,
            hasMetadata: true);

        Assert.True(ZLinkServiceWireCodec.TryDecodeInstanceSpotActivation(
            encoded,
            out var decoded,
            out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.True(decoded.HasMetadata);
        Assert.Equal(operation, decoded.Operation);

        var invalidRouteKind = encoded.ToArray();
        invalidRouteKind[5] = 3;
        Assert.False(ZLinkServiceWireCodec.TryDecodeInstanceSpotActivation(
            invalidRouteKind,
            out _,
            out var routeKindError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, routeKindError);

        Assert.False(ZLinkServiceWireCodec.TryDecodeInstanceSpotActivation(
            [.. encoded, 0],
            out _,
            out var trailingError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailingError);
    }

    [Fact]
    public void UserSpotOperationWireRoundTripsExactReservationCloseAndReplyTails()
    {
        var sourceRid = RoutingId.From("create-source");
        var targetRid = RoutingId.From("create-target");
        const string spotId = "created-spot";
        var reservation = new ObjectReservationFence(
            "reservation-1",
            "store-17",
            19,
            23,
            targetRid,
            29,
            "owner-b",
            31,
            1);
        var create = new UserSpotCreateOperation(
            37,
            new MeshOperationId(41, 43),
            sourceRid,
            47,
            spotId,
            "Sample.UserSpot",
            reservation,
            4_102_444_800_000);

        var encodedCreate = ZLinkServiceWireCodec.EncodeUserSpotCreate(create);
        Assert.True(ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
            encodedCreate,
            out var decodedCreate,
            out var createError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, createError);
        Assert.Equal(ServiceWireConstants.Command.UserSpotCreate, decodedCreate.Command);
        Assert.Equal(create, decodedCreate.Create);

        var close = new UserSpotCloseOperation(
            53,
            new MeshOperationId(59, 61),
            sourceRid,
            47,
            new UserSpotCloseFence(
                spotId,
                19,
                targetRid,
                29,
                23,
                "store-18"),
            4_102_444_800_000);
        var encodedClose = ZLinkServiceWireCodec.EncodeUserSpotClose(close);
        Assert.True(ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
            encodedClose,
            out var decodedClose,
            out var closeError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, closeError);
        Assert.Equal(ServiceWireConstants.Command.UserSpotClose, decodedClose.Command);
        Assert.Equal(close, decodedClose.Close);

        var createReply = ZLinkServiceWireCodec.EncodeUserSpotCreateReply(
            37,
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCreateCompletion(
                UserSpotCreateResult.Created,
                spotId,
                19));
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            createReply,
            out var decodedCreateReply,
            out var createReplyError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, createReplyError);
        Assert.True(ZLinkServiceWireCodec.TryDecodeUserSpotReply(
            decodedCreateReply,
            MeshOperationKind.UserSpotCreate,
            out var createCompletion,
            out var createTailError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, createTailError);
        Assert.Equal(
            new UserSpotCreateCompletion(UserSpotCreateResult.Created, spotId, 19),
            createCompletion);

        var closeReply = ZLinkServiceWireCodec.EncodeUserSpotCloseReply(
            53,
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCloseCompletion(true));
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            closeReply,
            out var decodedCloseReply,
            out var closeReplyError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, closeReplyError);
        Assert.True(ZLinkServiceWireCodec.TryDecodeUserSpotReply(
            decodedCloseReply,
            MeshOperationKind.UserSpotClose,
            out var closeCompletion,
            out var closeTailError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, closeTailError);
        Assert.Equal(new UserSpotCloseCompletion(true), closeCompletion);

        Assert.False(ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
            [.. encodedCreate, 0],
            out _,
            out var trailingError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TrailingByte, trailingError);
        Assert.False(ZLinkServiceWireCodec.TryDecodeUserSpotOperation(
            encodedClose.AsSpan(0, encodedClose.Length - 1),
            out _,
            out var truncatedError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.TruncatedField, truncatedError);
    }

    [Fact]
    public void StatefulWireRoundTripsExactSpotAndActorFences()
    {
        var nodeRid = RoutingId.From("wire-node");
        const string spotId = "wire-spot";
        const string sourceSpotId = "wire-source";
        const ulong deadlineUnixMs = 4_102_444_800_123;
        var spot = ZLinkServiceWireCodec.EncodeSpot(
            ServiceWireConstants.Command.SpotRequest,
            9,
            new MeshOperationId(7, 8),
            sourceSpotId,
            spotId,
            10,
            nodeRid,
            11,
            12,
            13,
            hasMetadata: true,
            messageFollowHopCount: 2,
            deadlineUnixMs: deadlineUnixMs);
        Assert.True(ZLinkServiceWireCodec.TryDecodeStateful(
            spot,
            "play",
            out var spotRecord,
            out var spotError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, spotError);
        Assert.Equal(9UL, spotRecord.Correlation);
        Assert.Equal(new MeshOperationId(7, 8), spotRecord.OperationId);
        Assert.Equal(sourceSpotId, spotRecord.SourceSpotId);
        Assert.Equal(spotId, spotRecord.TargetSpotId);
        Assert.Equal(10UL, spotRecord.TargetSpotGeneration);
        Assert.Equal(13UL, spotRecord.OwnerLeaseGeneration);
        Assert.Equal<byte>(2, spotRecord.MessageFollowHopCount);
        Assert.Equal(deadlineUnixMs, spotRecord.DeadlineUnixMs);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeSpot(
                ServiceWireConstants.Command.SpotRequest,
                9,
                new MeshOperationId(7, 8),
                sourceSpotId,
                spotId,
                10,
                nodeRid,
                11,
                12,
                13,
                hasMetadata: false,
                deadlineUnixMs: 0));

        var actor = new ActorRef("wire-actor", 13, "play", nodeRid);
        var actorBytes = ZLinkServiceWireCodec.EncodeActor(
            ServiceWireConstants.Command.ActorSend,
            0,
            new MeshOperationId(17, 18),
            actor,
            nodeRid,
            14,
            15,
            16,
            hasMetadata: false);
        Assert.True(ZLinkServiceWireCodec.TryDecodeStateful(
            actorBytes,
            "play",
            out var actorRecord,
            out var actorError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, actorError);
        Assert.Equal(actor, actorRecord.TargetActor);
        Assert.Equal(new MeshOperationId(17, 18), actorRecord.OperationId);
        Assert.Equal(14UL, actorRecord.TargetNodeGeneration);
        Assert.Equal(15UL, actorRecord.AuthorityOwnerGeneration);
        Assert.Equal(16UL, actorRecord.OwnerLeaseGeneration);

        var otherNodeRid = RoutingId.From("wire-node-other");
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeActor(
                ServiceWireConstants.Command.ActorSend,
                0,
                new MeshOperationId(17, 19),
                actor,
                otherNodeRid,
                14,
                15,
                16,
                hasMetadata: false));

        actorBytes.AsSpan(actorBytes.Length - sizeof(ulong)).Clear();
        Assert.False(ZLinkServiceWireCodec.TryDecodeStateful(
            actorBytes,
            "play",
            out _,
            out var zeroLeaseError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, zeroLeaseError);
    }

    [Fact]
    public async Task ActorPayloadUsesActorMailboxAndLifecycleUsesSpotMailbox()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "stateful-node");
        var actor = node.CreateActor("player-1");

        using var payload = Message.From(new byte[] { 1 });
        Assert.Equal(SubmitResult.Ok, node.SendToActor(actor, [payload]));

        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        Assert.Equal(2, ready.Count);
        Assert.Contains(
            Enumerable.Range(0, ready.Count),
            index => ready[index].OwnerKind == MeshOwnerKind.Actor
                     && ready[index].Domain == MeshReadyDomains.Application);
        Assert.Contains(
            Enumerable.Range(0, ready.Count),
            index => ready[index].OwnerKind == MeshOwnerKind.Spot
                     && ready[index].Domain == MeshReadyDomains.Application);

        var actorIndex = Enumerable.Range(0, ready.Count)
            .Single(index => ready[index].OwnerKind == MeshOwnerKind.Actor);
        using var actorClaim = ready.TakeClaim(actorIndex);
        using var received = new MeshReceiveBatch();
        Assert.True(actorClaim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.ActorSend, received[0].Kind);
        Assert.Equal(actor, ready[actorIndex].Actor);
        Assert.NotEqual(default, received[0].OperationId);
        Assert.Equal(node.Status().LifecycleGeneration, received[0].TargetNodeGeneration);
        Assert.True(node.TryGetActorAuthority(
            actor,
            out var authorityOwnerGeneration,
            out var ownerLeaseGeneration));
        Assert.Equal(authorityOwnerGeneration, received[0].AuthorityOwnerGeneration);
        Assert.Equal(ownerLeaseGeneration, received[0].OwnerLeaseGeneration);
        Assert.Equal(1UL, ownerLeaseGeneration);
    }

    [Fact]
    public async Task EntrySpot_RekeyKeepsActorOwnerAtTheLogicalEntrySpot()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "entry-rekey-node");
        var entrySpot = (ZLinkManagedSpot)node.EntrySpot();

        entrySpot.SetRoutingId(RoutingId.From("entry-rekey-logical-spot"));
        var actor = node.CreateActor("entry-rekey-actor");

        Assert.Same(entrySpot, node.EntrySpot());
        Assert.Equal("entry-rekey-logical-spot", entrySpot.SpotId);
        Assert.True(node.ActorLookup(actor.ActorId, out var location));
        Assert.Equal(entrySpot.SpotId, location.SpotId);
    }

    [Fact]
    public async Task RouteMesh_ApplicationHwm_AdmitsOneMailboxAndResumesAfterRelease()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "route-hwm-node");
        var budget = new ZLinkInboundDispatchBudget(1);
        node.SetInboundDispatchBudget(budget);
        var firstActor = node.CreateActor("route-hwm-first");
        var secondActor = node.CreateActor("route-hwm-second");
        DrainAndDispose(node);

        using var firstPayload = Message.From(new byte[] { 1 });
        using var secondPayload = Message.From(new byte[] { 2 });
        Assert.Equal(
            SubmitResult.Ok,
            node.SendToActor(firstActor, [firstPayload]));
        Assert.Equal(
            SubmitResult.Ok,
            node.SendToActor(secondActor, [secondPayload]));

        using (var ready = new MeshReadyBatch())
        {
            node.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            Assert.Equal(1, ready.Count);
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(MeshRecordKind.ActorSend, received[0].Kind);
            Assert.Equal(1UL, budget.PendingPayloadBytes);
        }

        Assert.Equal(0UL, budget.PendingPayloadBytes);
        using var resumed = new MeshReadyBatch();
        node.DrainReady(
            MeshReadyDomains.Application,
            resumed,
            RecvFlags.DontWait);
        Assert.Equal(1, resumed.Count);
        using var resumedClaim = resumed.TakeClaim(0);
        using var resumedReceive = new MeshReceiveBatch();
        Assert.True(resumedClaim.Receive(resumedReceive, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.ActorSend, resumedReceive[0].Kind);
    }

    [Fact]
    public async Task RouteMesh_Pump_DoesNotDrainSecondApplicationRecordAboveHwm()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "route-pump-hwm-node");
        var budget = new ZLinkInboundDispatchBudget(1);
        node.SetInboundDispatchBudget(budget);
        var entrySpot = node.EntrySpot();
        var firstActor = node.CreateActor("route-pump-hwm-first");
        var secondActor = node.CreateActor("route-pump-hwm-second");
        DrainAndDispose(node);

        await using var pump = new ZLinkMeshDispatchPump(
            node,
            new ZLinkMeshCompletionTable());
        pump.SetInboundDispatchBudget(budget);

        using var firstPayload = Message.From(new byte[] { 1 });
        using var secondPayload = Message.From(new byte[] { 2 });
        Assert.Equal(
            SubmitResult.Ok,
            node.SendToActor(firstActor, [firstPayload]));
        Assert.Equal(
            SubmitResult.Ok,
            node.SendToActor(secondActor, [secondPayload]));

        var dispatchCount = 0;
        var maximumPendingBytes = 0L;
        var dispatched = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        pump.SetDispatchHandler(
            entrySpot.RoutingId.ToString(),
            info =>
            {
                if (info.Event != ZLinkBackendSpotDispatchEvent.ActorReadable
                    || info.ActorParts is not { Count: > 0 } parts)
                    return;

                try
                {
                    var pendingBytes = checked((long)budget.PendingPayloadBytes);
                    while (true)
                    {
                        var observed = Volatile.Read(ref maximumPendingBytes);
                        if (pendingBytes <= observed
                            || Interlocked.CompareExchange(
                                ref maximumPendingBytes,
                                pendingBytes,
                                observed) == observed)
                            break;
                    }
                    Interlocked.Increment(ref dispatchCount);
                }
                finally
                {
                    foreach (var part in parts)
                        part.Message.Dispose();
                    info.ActorDispatchLease?.Dispose();
                    if (Volatile.Read(ref dispatchCount) == 2)
                        dispatched.TrySetResult(true);
                }
            });

        await dispatched.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.Equal(2, dispatchCount);
        Assert.Equal(1L, maximumPendingBytes);
        Assert.Equal(0UL, budget.PendingPayloadBytes);
    }

    [Fact]
    public async Task LogicalMulticastSnapshotsMatchingSpotMailboxes()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "multicast-node");
        var publisher = node.CreateSpot();
        var first = node.CreateSpot();
        var second = node.CreateSpot();
        first.SetSubscription("events", "room.updated");
        second.SetSubscription("events", "room.updated");

        using var payload = Message.From(new byte[] { 2 });
        publisher.Publish(
            "events",
            "room.updated",
            [payload]);

        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(2, ready.Count);
        foreach (var index in Enumerable.Range(0, ready.Count))
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(MeshRecordKind.SpotMulticast, received[0].Kind);
            Assert.Equal("events", received[0].ChannelName);
            Assert.Equal("room.updated", received[0].Topic);
        }
    }

    [Fact]
    public async Task LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "multicast-source");
        await using var light = NewNode(context, "multicast-light");
        await using var heavy = NewNode(context, "multicast-heavy");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://multicast-source-{suffix}";
        var lightEndpoint = $"inproc://multicast-light-{suffix}";
        var heavyEndpoint = $"inproc://multicast-heavy-{suffix}";
        source.SetBind(sourceEndpoint);
        light.SetBind(lightEndpoint);
        heavy.SetBind(heavyEndpoint);
        source.AddChannel("events");
        light.AddChannel("events");
        heavy.AddChannel("events");
        light.SetChannelWeight("events", 1);
        heavy.SetChannelWeight("events", 10_000);
        source.ConnectPeer(lightEndpoint, light.RoutingId);
        source.ConnectPeer(heavyEndpoint, heavy.RoutingId);
        light.ConnectPeer(sourceEndpoint, source.RoutingId);
        heavy.ConnectPeer(sourceEndpoint, source.RoutingId);
        source.Start();
        light.Start();
        heavy.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 2
                  && light.Status().AdmittedPeerCount == 1
                  && heavy.Status().AdmittedPeerCount == 1);

        var publisher = source.CreateSpot();
        var lightSubscriber = light.CreateSpot();
        var heavySubscriber = heavy.CreateSpot();
        lightSubscriber.SetSubscription("events", "room.updated");
        heavySubscriber.SetSubscription("events", "room.updated");
        using var payload = Message.From(new byte[] { 2 });
        publisher.Publish(
            "events",
            "room.updated",
            [payload]);

        await WaitUntilAsync(
            () => light.Status().PendingApplicationMessages == 1
                  && heavy.Status().PendingApplicationMessages == 1);
        Assert.Equal(
            MeshRecordKind.SpotMulticast,
            Assert.Single(DrainRecords(light)).Kind);
        Assert.Equal(
            MeshRecordKind.SpotMulticast,
            Assert.Single(DrainRecords(heavy)).Kind);
    }

    [Fact]
    public async Task JoinCommitsMembershipOnlyAfterAcceptedReply()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "join-node");
        var actor = node.CreateActor("player-2");
        DrainAndDispose(node);
        const string targetRid = "room-7";
        var target = node.GetOrCreateSpot(targetRid, out var created);
        Assert.True(created);

        var operation = node.JoinSpot(
            actor,
            node.RoutingId,
            targetRid,
            target.LifecycleGeneration);
        Assert.True(node.ActorLookup(actor.ActorId, out var before));
        Assert.NotEqual(targetRid, before.SpotId);

        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        var joinIndex = Enumerable.Range(0, ready.Count)
            .Single(index => ready[index].SpotId == targetRid);
        using var claim = ready.TakeClaim(joinIndex);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.SpotControl, received[0].Kind);
        Assert.Equal(MeshOperationKind.ActorJoin, received[0].OperationKind);
        Assert.Equal(
            SubmitResult.Ok,
            received[0].ReplyJoin(
                ActorJoinResult.Accepted,
                Array.Empty<Message>()));

        Assert.True(node.ActorLookup(actor.ActorId, out var after));
        Assert.Equal(targetRid, after.SpotId);
        Assert.Equal(before.MembershipEpoch + 1, after.MembershipEpoch);

        var completions = DrainRecords(node);
        var completion = Assert.Single(
            completions.Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));
        Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
        Assert.Equal(
            ActorJoinResult.Accepted,
            Assert.IsType<ActorJoinCompletion>(completion.JoinCompletion).JoinResult);
    }

    [Fact]
    public async Task ActorRequestHasOneTerminalWinnerAcrossDuplicateReplies()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "request-node");
        var actor = node.CreateActor("player-3");
        DrainAndDispose(node);

        using var request = Message.From(new byte[] { 3 });
        Assert.Equal(
            SubmitResult.Ok,
            node.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(1)));

        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        var inbound = received[0];
        Assert.Equal(operation.Low, inbound.ReplyRouteId);
        Parallel.For(
            0,
            32,
            _ => Assert.Equal(
                SubmitResult.Ok,
                inbound.Reply(Array.Empty<Message>())));

        var completions = DrainRecords(node);
        Assert.Single(
            completions.Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));
    }

    [Fact]
    public async Task RequestOperationCapacityBackpressuresBeforeAllocatingMoreWork()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        node.SetRoutingId(RoutingId.From("capacity-node"));
        var actor = node.CreateActor("capacity-actor");
        DrainAndDispose(node);

        using var request = Message.From(new byte[] { 4 });
        Assert.Equal(
            SubmitResult.Ok,
            node.RequestToActor(
                actor,
                [request],
                out var first,
                TimeSpan.FromSeconds(30)));
        Assert.NotEqual(default, first);

        Assert.Equal(
            SubmitResult.Backpressured,
            node.RequestToActor(
                actor,
                [request],
                out var rejected,
                TimeSpan.FromSeconds(30)));
        Assert.Equal(default, rejected);
    }

    [Fact]
    public async Task RemoteActorRequestCapacityRejectsBeforeTargetAdmission()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        caller.SetRoutingId(RoutingId.From("capacity-remote-caller"));
        await using var owner = NewNode(context, "capacity-remote-owner");
        owner.SetLocalOwnerLeaseGeneration(127);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://capacity-remote-caller-{suffix}";
        var ownerEndpoint = $"inproc://capacity-remote-owner-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.Start();
        owner.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 1
                                  && owner.Status().AdmittedPeerCount == 1);

        var actor = owner.CreateActor("capacity-remote-actor");
        DrainAndDispose(owner);
        Assert.True(owner.TryGetActorAuthority(
            actor,
            out var authorityOwnerGeneration,
            out var ownerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            owner.Status().LifecycleGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);

        using var request = Message.From(new byte[] { 131 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var admitted,
                TimeSpan.FromSeconds(30)));
        Assert.NotEqual(default, admitted);
        Assert.Equal(
            SubmitResult.Backpressured,
            caller.RequestToActor(
                actor,
                [request],
                out var rejected,
                TimeSpan.FromSeconds(30)));
        Assert.Equal(default, rejected);

        await WaitUntilAsync(() =>
            owner.Status().PendingApplicationMessages == 1);
        using var ready = new MeshReadyBatch();
        owner.DrainReady(
            MeshReadyDomains.Application,
            ready,
            RecvFlags.DontWait);
        Assert.Equal(1, ready.Count);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(1, received.Count);
        Assert.Equal(admitted, received[0].OperationId);
        Assert.Equal(admitted.Low, received[0].ReplyRouteId);
    }

    [Fact]
    public async Task RemoteActorStaleAuthorityReturnsOneTerminalForTheOriginalOperation()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "stale-authority-caller");
        await using var owner = NewNode(context, "stale-authority-owner");
        owner.SetLocalOwnerLeaseGeneration(137);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://stale-authority-caller-{suffix}";
        var ownerEndpoint = $"inproc://stale-authority-owner-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "stale-authority-caller-owner",
            1,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        caller.Start();
        owner.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 1
                                  && owner.Status().AdmittedPeerCount == 1);

        var actor = owner.CreateActor("stale-authority-actor");
        DrainAndDispose(owner);
        Assert.True(owner.TryGetActorAuthority(
            actor,
            out var sourceAuthorityOwnerGeneration,
            out var sourceOwnerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            owner.Status().LifecycleGeneration,
            sourceAuthorityOwnerGeneration,
            sourceOwnerLeaseGeneration);

        // The caller submits with the immutable source fence while the owner
        // has already advanced to the target authority.
        owner.SetActorAuthority(
            actor,
            checked(sourceAuthorityOwnerGeneration + 1));
        using var request = Message.From(new byte[] { 139 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(3)));

        await WaitUntilAsync(() =>
            caller.Status().PendingInfrastructureMessages > 0);
        var completion = Assert.Single(DrainRecords(caller).Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == operation));
        Assert.Equal((int)RequestResult.Conflict, completion.TerminalResult);
        Assert.Equal(
            (int)ServiceWireConstants.FrameworkErrorCode.ActorLocationStale,
            completion.FailureErrno);
        Assert.Equal(0UL, owner.Status().PendingApplicationMessages);

        var lateRelay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            operation,
            operation.Low,
            new ZLinkServiceWireCodec.RelocationWireId(149, 151),
            157,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "stale-authority-target-owner",
                163,
                RoutingId.From("stale-authority-target"),
                167,
                "stale-authority-root"),
            173,
            179,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        var latePayload = new[] { Message.From(new byte[] { 181 }) };
        var duplicate = caller.TryCompleteRelocationReply(
            lateRelay,
            latePayload);
        Assert.Equal(
            ZLinkRelocationReplyCompletionState.AlreadyTerminal,
            duplicate.State);
        Assert.Equal(requestSource, duplicate.RequestSource);
        Assert.Throws<ObjectDisposedException>(
            () => latePayload[0].AsReadOnlySpan());
        await Task.Delay(50);
        Assert.DoesNotContain(
            DrainRecords(caller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == operation);
    }

    [Fact]
    public async Task RemoteActorStaleAuthorityUsesTheActiveFollowerBeforeAStaleTerminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "active-follower-caller");
        await using var owner = NewNode(context, "active-follower-owner");
        owner.SetLocalOwnerLeaseGeneration(191);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "active-follower-caller-owner",
            181,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        var ownerBackend = new ZLinkBackendSpotNodeWrapper(owner);
        ownerBackend.ObserveRequestSourceFence(requestSource);
        var follower = new CapturingBackendActorMessageFollowHandler(
            acceptsOwnership: true);
        ownerBackend.SetActorMessageFollowIngressHandler(follower.TryFollow);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://active-follower-caller-{suffix}";
        var ownerEndpoint = $"inproc://active-follower-owner-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.Start();
        owner.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 1
                                  && owner.Status().AdmittedPeerCount == 1);

        var actor = owner.CreateActor("active-follower-actor");
        DrainAndDispose(owner);
        Assert.True(owner.TryGetActorAuthority(
            actor,
            out var sourceAuthorityOwnerGeneration,
            out var sourceOwnerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            owner.Status().LifecycleGeneration,
            sourceAuthorityOwnerGeneration,
            sourceOwnerLeaseGeneration);
        owner.SetActorAuthority(
            actor,
            checked(sourceAuthorityOwnerGeneration + 1));

        using var request = Message.From(new byte[] { 193 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(3)));

        await WaitUntilAsync(() =>
            caller.Status().PendingInfrastructureMessages > 0);
        var completion = Assert.Single(DrainRecords(caller).Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == operation));
        Assert.Equal((int)RequestResult.Ok, completion.TerminalResult);
        Assert.Equal(1, follower.Count);
        Assert.Equal(operation, follower.LastRoute.OperationId);
        Assert.Equal(operation.Low, follower.LastRoute.ReplyRequestId);
        Assert.Equal(
            sourceAuthorityOwnerGeneration,
            follower.LastRoute.AuthorityOwnerGeneration);
        Assert.Equal(
            sourceOwnerLeaseGeneration,
            follower.LastRoute.OwnerLeaseGeneration);
        Assert.Equal(
            owner.Status().LifecycleGeneration,
            follower.LastRoute.TargetNodeGeneration);
        Assert.Equal(operation, follower.LastRoute.OperationId);
        Assert.Equal(operation.Low, follower.LastRoute.ReplyRequestId);
        Assert.Equal(requestSource, follower.LastRequestSource);
        Assert.Empty(follower.LastApplicationMetadata);
        Assert.Equal([193], follower.LastPayload);
        Assert.True(follower.DisposedByHandler);
        Assert.All(
            follower.LastMessages,
            message => Assert.Throws<ObjectDisposedException>(
                () => message.AsReadOnlySpan()));
        Assert.Equal(0UL, owner.Status().PendingApplicationMessages);

        await Task.Delay(50);
        Assert.DoesNotContain(
            DrainRecords(caller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == operation);
    }

    [Fact]
    public async Task RemoteActorStaleAuthorityDisposesRejectedFollowerPartsAndReturnsOneStaleTerminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "rejected-follower-caller");
        await using var owner = NewNode(context, "rejected-follower-owner");
        owner.SetLocalOwnerLeaseGeneration(211);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "rejected-follower-caller-owner",
            201,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        var ownerBackend = new ZLinkBackendSpotNodeWrapper(owner);
        ownerBackend.ObserveRequestSourceFence(requestSource);
        var follower = new CapturingBackendActorMessageFollowHandler(
            acceptsOwnership: false);
        ownerBackend.SetActorMessageFollowIngressHandler(follower.TryFollow);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://rejected-follower-caller-{suffix}";
        var ownerEndpoint = $"inproc://rejected-follower-owner-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.Start();
        owner.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 1
                                  && owner.Status().AdmittedPeerCount == 1);

        var actor = owner.CreateActor("rejected-follower-actor");
        DrainAndDispose(owner);
        Assert.True(owner.TryGetActorAuthority(
            actor,
            out var sourceAuthorityOwnerGeneration,
            out var sourceOwnerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            owner.Status().LifecycleGeneration,
            sourceAuthorityOwnerGeneration,
            sourceOwnerLeaseGeneration);
        owner.SetActorAuthority(
            actor,
            checked(sourceAuthorityOwnerGeneration + 1));

        using var request = Message.From(new byte[] { 213 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(3)));

        await WaitUntilAsync(() =>
            caller.Status().PendingInfrastructureMessages > 0);
        var completion = Assert.Single(DrainRecords(caller).Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == operation));
        Assert.Equal((int)RequestResult.Conflict, completion.TerminalResult);
        Assert.Equal(
            (int)ServiceWireConstants.FrameworkErrorCode.ActorLocationStale,
            completion.FailureErrno);
        Assert.Equal(1, follower.Count);
        Assert.False(follower.DisposedByHandler);
        Assert.Equal(operation, follower.LastRoute.OperationId);
        Assert.Equal(operation.Low, follower.LastRoute.ReplyRequestId);
        Assert.Equal(requestSource, follower.LastRequestSource);
        Assert.Equal([213], follower.LastPayload);
        Assert.All(
            follower.LastMessages,
            message => Assert.Throws<ObjectDisposedException>(
                () => message.AsReadOnlySpan()));

        await Task.Delay(50);
        Assert.DoesNotContain(
            DrainRecords(caller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == operation);
    }

    [Fact]
    public async Task ActorMessageFollowIngressAdapterPreservesExactRouteMetadataAndOwnership()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "follow-adapter-node");
        var pump = new ZLinkMeshDispatchPump(
            node,
            new ZLinkMeshCompletionTable());
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "follow-adapter-source-owner",
            223,
            RoutingId.From("follow-adapter-source"),
            227);
        pump.ObserveRequestSourceFence(source);
        var adapter =
            new ZLinkBackendSpotNodeWrapper.ActorMessageFollowIngressAdapter(
                pump);
        var follower = new CapturingBackendActorMessageFollowHandler(
            acceptsOwnership: true,
            reply: false);
        adapter.SetHandler(follower.TryFollow);
        var operation = new MeshOperationId(229, 233);
        var actor = new ActorRef(
            "follow-adapter-actor",
            239,
            "mesh",
            RoutingId.From("follow-adapter-owner"));
        var metadata = new byte[] { 241, 251 };
        var messages = new[]
        {
            Message.From(new byte[] { 2, 3 }),
            Message.From(new byte[] { 5, 7 })
        };

        Assert.True(adapter.TryFollow(new ActorMessageFollowIngress(
            source.NodeRid,
            source.NodeGeneration,
            "source-spot",
            actor,
            operation,
            ReplyRouteId: 257,
            TargetNodeGeneration: 263,
            AuthorityOwnerGeneration: 269,
            OwnerLeaseGeneration: 271,
            MessageFollowHopCount: 3,
            DeadlineUnixMs: 277,
            ApplicationMetadata: metadata,
            Parts: messages,
            Reply: static (_, _) => SubmitResult.Ok)));

        Assert.Equal(1, follower.Count);
        Assert.Equal(operation, follower.LastRoute.OperationId);
        Assert.Equal(257UL, follower.LastRoute.ReplyRequestId);
        Assert.Equal(263UL, follower.LastRoute.TargetNodeGeneration);
        Assert.Equal(269UL, follower.LastRoute.AuthorityOwnerGeneration);
        Assert.Equal(271UL, follower.LastRoute.OwnerLeaseGeneration);
        Assert.Equal((byte)3, follower.LastRoute.MessageFollowHopCount);
        Assert.Equal(277UL, follower.LastRoute.DeadlineUnixMs);
        Assert.Equal(source, follower.LastRequestSource);
        Assert.Equal(metadata, follower.LastApplicationMetadata);
        Assert.Equal([2, 3, 5, 7], follower.LastPayload);
        Assert.True(follower.DisposedByHandler);
        Assert.All(
            messages,
            message => Assert.Throws<ObjectDisposedException>(
                () => message.AsReadOnlySpan()));

        var admissionCalls = 0;
        adapter.SetAdmission(_ =>
        {
            admissionCalls++;
            return false;
        });
        var encodedPayload = ZLinkApplicationPayloadEnvelopeCodec
            .EncodeFrameworkMultipart(
                new ReadOnlyMemory<byte>[] { new byte[] { 11, 13, 17 } });
        var rejected = new ActorMessageFollowIngress(
            source.NodeRid,
            source.NodeGeneration,
            "source-spot",
            actor,
            new MeshOperationId(281, 283),
            ReplyRouteId: 287,
            TargetNodeGeneration: 293,
            AuthorityOwnerGeneration: 307,
            OwnerLeaseGeneration: 311,
            MessageFollowHopCount: 1,
            DeadlineUnixMs: 313,
            ApplicationMetadata: ReadOnlyMemory<byte>.Empty,
            Parts: Array.Empty<Message>(),
            Reply: null)
        {
            EncodedPayload = encodedPayload
        };

        Assert.False(adapter.TryFollow(rejected));
        Assert.Equal(1, admissionCalls);
        Assert.Equal(1, follower.Count);

        var ownershipAdapter =
            new ZLinkBackendSpotNodeWrapper.ActorMessageFollowIngressAdapter(
                pump);
        var ownershipFollower = new CapturingBackendActorMessageFollowHandler(
            acceptsOwnership: true,
            reply: false);
        ownershipAdapter.SetHandler(ownershipFollower.TryFollow);
        ownershipAdapter.SetAdmission(_ => true);
        using (var metadataSource = Message.From(new byte[] { 19, 23, 29 }))
        {
            var admitted = rejected with
            {
                ApplicationMetadataSource = metadataSource
            };

            Assert.True(ownershipAdapter.TryFollow(admitted));
        }
        Assert.Equal([19, 23, 29], ownershipFollower.LastApplicationMetadata);
    }

    [Fact]
    public void MailboxAccountingSeparatesPayloadHwmFromRetainedMailboxBytes()
    {
        var record = new MeshReceiveRecord(
            MeshRecordKind.ActorSend,
            MeshReadyDomains.Application,
            default,
            string.Empty,
            0,
            default,
            default,
            MeshOperationKind.NodeRequest,
            null,
            null,
            new byte[] { 19, 23, 29 },
            0,
            1,
            0,
            0,
            null);
        var queued = new ZLinkMeshQueuedRecord(
            record,
            new[] { Message.From(new byte[] { 31, 37 }) });

        try
        {
            Assert.Equal(2UL, queued.PayloadBytes);
            Assert.Equal(
                2UL + 3UL + ZLinkMeshQueuedRecord.FixedRecordBytes,
                queued.PendingBytes);
        }
        finally
        {
            queued.Dispose();
        }
    }

    [Fact]
    public async Task CompletionOverflowUsesInternalSinkWhenMarkerExceedsByteBudget()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "completion-overflow-node");
        node.MailboxByteBudget = 1024;
        var actor = node.CreateActor("completion-overflow-actor");
        DrainAndDispose(node);

        var overflow = new List<MeshReceiveRecord>();
        node.SetCompletionOverflowHandlerCore((record, parts) =>
        {
            Assert.Empty(parts);
            overflow.Add(record);
        });

        using var request = Message.From(new byte[] { 41 });
        Assert.Equal(
            SubmitResult.Ok,
            node.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(2)));

        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            node.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            if (ready.Count == 0)
                return false;
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            if (!claim.Receive(received, RecvFlags.DontWait))
                return false;

            node.MailboxByteBudget = 1;
            Assert.Equal(
                SubmitResult.Ok,
                received[0].Reply(Array.Empty<Message>()));
            return true;
        });

        var failure = Assert.Single(overflow);
        Assert.Equal(operation, failure.OperationId);
        Assert.Equal(
            (int)RequestResult.Backpressured,
            failure.TerminalResult);
    }

    [Fact]
    public async Task SessionBindingUsesExactActorGenerationAndRelaysToActorTurn()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "session-node");
        var actor = node.CreateActor("player-5");
        DrainAndDispose(node);
        await using var stream = context.CreateStreamSocket();
        await using var sessions = node.CreateStreamSessionService(stream);
        sessions.Start();
        var sessionRid = RoutingId.From("session-1");

        Assert.Equal(
            SubmitResult.Ok,
            sessions.BindActor(
                sessionRid,
                actor,
                out var bindOperation,
                TimeSpan.FromSeconds(1)));
        Assert.NotEqual(default, bindOperation);
        var binding = Assert.Single(sessions.Bindings(sessionRid));
        Assert.Equal(actor, binding.Actor);
        DrainAndDispose(node);

        using var payload = Message.From(new byte[] { 6 });
        Assert.Equal(
            SubmitResult.Ok,
            sessions.SendToActor(sessionRid, actor, [payload]));
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(MeshRecordKind.ActorSend, received[0].Kind);

        var stale = new ActorRef(
            actor.ActorId,
            actor.ObjectGeneration + 1,
            actor.MeshName,
            actor.NodeRid);
        Assert.Equal(
            SubmitResult.NotFound,
            sessions.SendToActor(sessionRid, stale, [payload]));
    }

    [Fact]
    public async Task InstanceStyleSpotKeepsOneGenerationAndRejectsStaleFence()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "instance-node");
        const string spotId = "instance-cart-1";
        var first = node.GetOrCreateSpot(spotId, out var firstCreated);
        var second = node.GetOrCreateSpot(spotId, out var secondCreated);
        Assert.True(firstCreated);
        Assert.False(secondCreated);
        Assert.Equal(first.LifecycleGeneration, second.LifecycleGeneration);

        using var payload = Message.From(new byte[] { 7 });
        Assert.Equal(
            SubmitResult.InvalidState,
            first.SendToSpot(
                node.RoutingId,
                spotId,
                first.LifecycleGeneration + 1,
                [payload]));
        Assert.Equal(
            SubmitResult.Ok,
            first.SendToSpot(
                node.RoutingId,
                spotId,
                first.LifecycleGeneration,
                [payload]));
        DrainAndDispose(node);
        await first.DisposeAsync();
        var reactivated = node.GetOrCreateSpot(spotId, out var reactivatedCreated);
        Assert.True(reactivatedCreated);
        Assert.NotEqual(
            first.LifecycleGeneration,
            reactivated.LifecycleGeneration);
    }

    [Fact]
    public async Task ObservedAuthorityAcceptsFullWidthEntrySpotGeneration()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "full-width-entry-source");

        // Entry Spot routing uses the native MeshNode lifecycle generation,
        // which is not the public SpotRef object-generation range.
        source.ObserveSpotAuthority(
            RoutingId.From("full-width-entry-target"),
            "full-width-entry-target-spot",
            ulong.MaxValue,
            ulong.MaxValue,
            ulong.MaxValue,
            ulong.MaxValue);
    }

    [Fact]
    public async Task RemoteSpotAndActorDispatchPreserveExactOwnerMailbox()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "stateful-source");
        await using var target = NewNode(context, "stateful-target");
        source.SetLocalOwnerLeaseGeneration(17);
        target.SetLocalOwnerLeaseGeneration(17);
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://stateful-source-{suffix}";
        var targetEndpoint = $"inproc://stateful-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        source.Start();
        target.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        var actor = target.CreateActor("remote-player");
        const string spotId = "remote-room";
        var spot = (ZLinkManagedSpot)target.GetOrCreateSpot(spotId, out _);
        DrainAndDispose(target);

        using var payload = Message.From(new byte[] { 8 });
        Assert.True(target.TryGetActorAuthority(
            actor,
            out var actorAuthority,
            out var actorOwnerLeaseGeneration));
        Assert.Equal(17UL, actorOwnerLeaseGeneration);
        source.ObserveActorAuthority(
            actor,
            checked(target.Status().LifecycleGeneration + 1),
            actorAuthority,
            17);
        source.ObserveSpotAuthority(
            target.RoutingId,
            spotId,
            spot.LifecycleGeneration,
            checked(target.Status().LifecycleGeneration + 1),
            spot.AuthorityOwnerGeneration,
            17);
        Assert.Equal(SubmitResult.NotFound, source.SendToActor(actor, [payload]));
        Assert.Equal(
            SubmitResult.NotFound,
            source.EntrySpot().SendToSpot(
                target.RoutingId,
                spotId,
                spot.LifecycleGeneration,
                [payload]));

        source.ObserveActorAuthority(
            actor,
            target.Status().LifecycleGeneration,
            actorAuthority,
            17);
        source.ObserveSpotAuthority(
            target.RoutingId,
            spotId,
            spot.LifecycleGeneration,
            target.Status().LifecycleGeneration,
            spot.AuthorityOwnerGeneration,
            17);
        Assert.Equal(SubmitResult.Ok, source.SendToActor(actor, [payload]));
        Assert.Equal(
            SubmitResult.Ok,
            source.EntrySpot().SendToSpot(
                target.RoutingId,
                spotId,
                spot.LifecycleGeneration,
                [payload]));

        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count == 2;
        });
        using var finalReady = new MeshReadyBatch();
        target.DrainReady(
            MeshReadyDomains.Application,
            finalReady,
            RecvFlags.DontWait);
        Assert.Contains(
            Enumerable.Range(0, finalReady.Count),
            index => finalReady[index].OwnerKind == MeshOwnerKind.Actor);
        Assert.Contains(
            Enumerable.Range(0, finalReady.Count),
            index => finalReady[index].SpotId == spotId);
        finalReady.Reset();
        DrainAndDispose(target);

        Assert.Equal(
            SubmitResult.Ok,
            source.RequestToActor(
                actor,
                [payload],
                out var operation,
                TimeSpan.FromSeconds(1)));
        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count == 1;
        });
        using (var requestReady = new MeshReadyBatch())
        {
            target.DrainReady(
                MeshReadyDomains.Application,
                requestReady,
                RecvFlags.DontWait);
            using var requestClaim = requestReady.TakeClaim(0);
            using var requestBatch = new MeshReceiveBatch();
            Assert.True(requestClaim.Receive(requestBatch, RecvFlags.DontWait));
            Assert.Equal(MeshRecordKind.ActorRequest, requestBatch[0].Kind);
            Assert.Equal(
                SubmitResult.Ok,
                requestBatch[0].Reply(Array.Empty<Message>()));
        }
        await WaitUntilAsync(() =>
            source.Status().PendingInfrastructureMessages > 0);
        var completions = DrainRecords(source);
        Assert.Single(
            completions.Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));
    }

    [Fact]
    public async Task RelocatedActorReplyCompletesTheOriginalRemoteCallerExactlyOnce()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        caller.SetRoutingId(RoutingId.From("relocated-reply-caller"));
        await using var owner = NewNode(context, "relocated-reply-owner");
        caller.SetLocalOwnerLeaseGeneration(41);
        owner.SetLocalOwnerLeaseGeneration(43);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://relocated-reply-caller-{suffix}";
        var ownerEndpoint = $"inproc://relocated-reply-owner-{suffix}";
        caller.SetBind(callerEndpoint);
        owner.SetBind(ownerEndpoint);
        caller.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(callerEndpoint, caller.RoutingId);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            41,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        var callerBackend = new ZLinkBackendSpotNodeWrapper(caller);
        caller.SetRelocationReplyRelayTarget(
            new ZLinkRelocationReplyTarget(callerBackend));
        caller.Start();
        owner.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 1
                                  && owner.Status().AdmittedPeerCount == 1);

        var actor = owner.CreateActor("relocated-reply-actor");
        DrainAndDispose(owner);
        Assert.True(owner.TryGetActorAuthority(
            actor,
            out var authorityOwnerGeneration,
            out var ownerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            owner.Status().LifecycleGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);

        using var request = Message.From(new byte[] { 31 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var operation,
                TimeSpan.FromSeconds(3)));

        MeshReceiveRecord inbound = default;
        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            owner.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            if (ready.Count == 0) return false;
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            if (!claim.Receive(received, RecvFlags.DontWait)) return false;
            inbound = received[0];
            return true;
        });
        Assert.NotEqual(caller.RoutingId, owner.RoutingId);
        Assert.Equal(MeshRecordKind.ActorRequest, inbound.Kind);
        Assert.Equal(operation, inbound.OperationId);
        Assert.Equal(operation.Low, inbound.ReplyRouteId);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            operation,
            inbound.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(47, 53),
            71,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator-owner",
                59,
                owner.RoutingId,
                owner.Status().LifecycleGeneration,
                "relocation-root"),
            61,
            67,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        Assert.NotEqual(
            relay.TargetAttemptGeneration,
            relay.Coordinator.NodeGeneration);
        var response = new[] { Message.From(new byte[] { 37 }) };
        try
        {
            var firstAck = await owner.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.TerminalReceived,
                firstAck.Status);
            Assert.Equal(requestSource, firstAck.RequestSource);

            await WaitUntilAsync(() =>
                caller.Status().PendingInfrastructureMessages > 0);
            var completions = DrainRecords(caller);
            Assert.Single(completions.Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));

            // A lost ACK causes the coordinator to resend the same immutable
            // relay. The caller acknowledges its tombstone without publishing
            // a second completion.
            var duplicateAck = await owner.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.AlreadyTerminal,
                duplicateAck.Status);
            Assert.Equal(requestSource, duplicateAck.RequestSource);
            await Task.Delay(50);
            Assert.DoesNotContain(
                DrainRecords(caller),
                record => record.Kind == MeshRecordKind.Completion
                          && record.OperationId == operation);
            Assert.Equal(
                SubmitResult.Ok,
                caller.RequestToActor(
                    actor,
                    [request],
                    out var nextOperation,
                    TimeSpan.FromSeconds(3)));
            Assert.NotEqual(default, nextOperation);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(response);
        }
    }

    [Fact]
    public async Task CallerTerminalPathsRetainTheExactRelocationReplyTombstone()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = NewNode(context, "reply-terminal-caller");
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "reply-terminal-owner",
            1,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        var actor = caller.CreateActor("reply-terminal-actor");
        DrainAndDispose(caller);
        using var request = Message.From(new byte[] { 101 });

        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var completedOperation,
                TimeSpan.FromSeconds(1)));
        using (var ready = new MeshReadyBatch())
        {
            caller.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(
                SubmitResult.Ok,
                received[0].Reply(Array.Empty<Message>()));
        }
        Assert.Contains(
            DrainRecords(caller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == completedOperation);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            completedOperation,
            completedOperation.Low,
            new ZLinkServiceWireCodec.RelocationWireId(103, 107),
            109,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "reply-terminal-target",
                113,
                RoutingId.From("reply-terminal-target"),
                127,
                "reply-terminal-root"),
            131,
            137,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        var duplicatePayload = new[] { Message.From(new byte[] { 139 }) };
        var duplicate = caller.TryCompleteRelocationReply(
            relay,
            duplicatePayload);
        Assert.Equal(
            ZLinkRelocationReplyCompletionState.AlreadyTerminal,
            duplicate.State);
        Assert.Equal(requestSource, duplicate.RequestSource);
        Assert.Throws<ObjectDisposedException>(
            () => duplicatePayload[0].AsReadOnlySpan());

        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [request],
                out var timedOutOperation,
                TimeSpan.FromMilliseconds(30)));
        await WaitUntilAsync(() =>
            caller.Status().PendingInfrastructureMessages > 0);
        Assert.Contains(
            DrainRecords(caller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == timedOutOperation
                      && record.TerminalResult == (int)RequestResult.TimedOut);

        var timedOutPayload = new[] { Message.From(new byte[] { 149 }) };
        var timedOutDuplicate = caller.TryCompleteRelocationReply(
            relay with
            {
                OperationId = timedOutOperation,
                ReplyRouteId = timedOutOperation.Low,
                Sequence = 151
            },
            timedOutPayload);
        Assert.Equal(
            ZLinkRelocationReplyCompletionState.AlreadyTerminal,
            timedOutDuplicate.State);
        Assert.Equal(requestSource, timedOutDuplicate.RequestSource);
        Assert.Throws<ObjectDisposedException>(
            () => timedOutPayload[0].AsReadOnlySpan());
    }

    [Fact]
    public async Task RelocatedUserSpotReplyCompletesTheOriginalRemoteCallerExactlyOnce()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        caller.SetRoutingId(RoutingId.From("relocated-spot-caller"));
        await using var oldOwner = NewNode(context, "relocated-spot-old-owner");
        await using var target = NewNode(context, "relocated-spot-target");
        caller.SetLocalOwnerLeaseGeneration(131);
        oldOwner.SetLocalOwnerLeaseGeneration(137);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://relocated-spot-caller-{suffix}";
        var oldOwnerEndpoint = $"inproc://relocated-spot-old-owner-{suffix}";
        var targetEndpoint = $"inproc://relocated-spot-target-{suffix}";
        caller.SetBind(callerEndpoint);
        oldOwner.SetBind(oldOwnerEndpoint);
        target.SetBind(targetEndpoint);
        caller.ConnectPeer(oldOwnerEndpoint, oldOwner.RoutingId);
        oldOwner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(callerEndpoint, caller.RoutingId);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "relocated-spot-caller-owner",
            131,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        var replyTarget = new ZLinkRelocationReplyTarget(
            new ZLinkBackendSpotNodeWrapper(caller));
        caller.SetRelocationReplyRelayTarget(replyTarget);
        caller.Start();
        oldOwner.Start();
        target.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 2
                                  && oldOwner.Status().AdmittedPeerCount == 1
                                  && target.Status().AdmittedPeerCount == 1);

        const string spotId = "relocated-user-spot";
        var spot = (ZLinkManagedSpot)oldOwner.GetOrCreateSpot(spotId, out _);
        DrainAndDispose(oldOwner);
        caller.ObserveSpotAuthority(
            oldOwner.RoutingId,
            spotId,
            spot.LifecycleGeneration,
            oldOwner.Status().LifecycleGeneration,
            spot.AuthorityOwnerGeneration,
            137);

        using var request = Message.From(new byte[] { 139 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.EntrySpot().RequestToSpot(
                oldOwner.RoutingId,
                spotId,
                spot.LifecycleGeneration,
                [request],
                out var operation,
                TimeSpan.FromSeconds(3)));
        Assert.Equal(
            SubmitResult.Backpressured,
            caller.EntrySpot().RequestToSpot(
                oldOwner.RoutingId,
                spotId,
                spot.LifecycleGeneration,
                [request],
                out var rejected,
                TimeSpan.FromSeconds(3)));
        Assert.Equal(default, rejected);

        MeshReceiveRecord inbound = default;
        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            oldOwner.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            if (ready.Count == 0) return false;
            using var claim = ready.TakeClaim(0);
            using var received = new MeshReceiveBatch();
            if (!claim.Receive(received, RecvFlags.DontWait)) return false;
            inbound = received[0];
            return true;
        });
        Assert.NotEqual(caller.RoutingId, oldOwner.RoutingId);
        Assert.NotEqual(oldOwner.RoutingId, target.RoutingId);
        Assert.Equal(MeshRecordKind.SpotRequest, inbound.Kind);
        Assert.Equal(operation, inbound.OperationId);
        Assert.Equal(operation.Low, inbound.ReplyRouteId);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            operation,
            inbound.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(149, 151),
            target.Status().LifecycleGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "relocated-spot-target-owner",
                157,
                target.RoutingId,
                target.Status().LifecycleGeneration,
                "relocated-spot-root"),
            163,
            167,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        var response = new[] { Message.From(new byte[] { 173 }) };
        try
        {
            var unauthenticatedPayload = new[] { Message.From(new byte[] { 171 }) };
            var unauthenticatedAck = await replyTarget.RelayAsync(
                relay,
                target.RoutingId,
                checked(target.Status().LifecycleGeneration + 1),
                unauthenticatedPayload,
                CancellationToken.None);
            Assert.Null(unauthenticatedAck);
            Assert.Throws<ObjectDisposedException>(() =>
                unauthenticatedPayload[0].AsReadOnlySpan());

            var firstAck = await target.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.TerminalReceived,
                firstAck.Status);
            Assert.Equal(requestSource, firstAck.RequestSource);

            await WaitUntilAsync(() =>
                caller.Status().PendingInfrastructureMessages > 0);
            var completion = Assert.Single(DrainRecords(caller).Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));
            Assert.Equal(MeshOperationKind.SpotRequest, completion.OperationKind);

            var duplicateAck = await target.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.AlreadyTerminal,
                duplicateAck.Status);
            await Task.Delay(50);
            Assert.DoesNotContain(
                DrainRecords(caller),
                record => record.Kind == MeshRecordKind.Completion
                          && record.OperationId == operation);
            Assert.Equal(
                SubmitResult.Ok,
                caller.EntrySpot().RequestToSpot(
                    oldOwner.RoutingId,
                    spotId,
                    spot.LifecycleGeneration,
                    [request],
                    out var nextOperation,
                    TimeSpan.FromSeconds(3)));
            Assert.NotEqual(default, nextOperation);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(response);
        }
    }

    [Fact]
    public async Task RelocationReplyFromAnOldCallerLifecycleCannotCompleteAReusedRoute()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        const string rid = "restarted-reply-caller";
        MeshOperationId oldOperation;
        ZLinkServiceWireCodec.RequestSourceFence oldSource;
        await using (var oldCaller = NewNode(context, rid))
        {
            oldSource = new ZLinkServiceWireCodec.RequestSourceFence(
                "old-caller-owner",
                71,
                oldCaller.RoutingId,
                oldCaller.Status().LifecycleGeneration);
            oldCaller.SetLocalRequestSourceFence(oldSource);
            var actor = oldCaller.CreateActor("old-caller-actor");
            DrainAndDispose(oldCaller);
            using var request = Message.From(new byte[] { 73 });
            Assert.Equal(
                SubmitResult.Ok,
                oldCaller.RequestToActor(
                    actor,
                    [request],
                    out oldOperation,
                    TimeSpan.FromSeconds(30)));
        }

        await using var restartedCaller = NewNode(context, rid);
        Assert.NotEqual(
            oldOperation.High,
            restartedCaller.Status().LifecycleGeneration);
        var restartedSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "new-caller-owner",
            79,
            restartedCaller.RoutingId,
            restartedCaller.Status().LifecycleGeneration);
        restartedCaller.SetLocalRequestSourceFence(restartedSource);
        var restartedActor = restartedCaller.CreateActor("new-caller-actor");
        DrainAndDispose(restartedCaller);
        using var restartedRequest = Message.From(new byte[] { 83 });
        Assert.Equal(
            SubmitResult.Ok,
            restartedCaller.RequestToActor(
                restartedActor,
                [restartedRequest],
                out var restartedOperation,
                TimeSpan.FromSeconds(30)));
        Assert.Equal(oldOperation.Low, restartedOperation.Low);
        Assert.NotEqual(oldOperation.High, restartedOperation.High);

        var oldRelay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            oldOperation,
            oldOperation.Low,
            new ZLinkServiceWireCodec.RelocationWireId(89, 97),
            101,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "old-coordinator",
                103,
                RoutingId.From("old-coordinator-node"),
                101,
                "old-root"),
            107,
            109,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        var stalePayload = new[] { Message.From(new byte[] { 113 }) };
        try
        {
            var completion = restartedCaller.TryCompleteRelocationReply(
                oldRelay,
                stalePayload);
            Assert.Equal(
                ZLinkRelocationReplyCompletionState.NotFound,
                completion.State);
            Assert.Equal(
                default,
                completion.RequestSource);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(stalePayload);
        }

        using var ready = new MeshReadyBatch();
        restartedCaller.DrainReady(
            MeshReadyDomains.Application,
            ready,
            RecvFlags.DontWait);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(
            SubmitResult.Ok,
            received[0].Reply(Array.Empty<Message>()));
        Assert.Contains(
            DrainRecords(restartedCaller),
            record => record.Kind == MeshRecordKind.Completion
                      && record.OperationId == restartedOperation);
    }

    [Fact]
    public async Task LocalSpotCatalogRejectsPreparedActivationAfterSpotCapacityIsReached()
    {
        ProductionUserSpot.Reset();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var node = options.AddRouteMesh("objects")
                .Listen($"tcp://127.0.0.1:{FindFreeTcpPort()}")
                .SetSpotLimit(1);
            node.Objects()
                .Server()
                .AddSpotFactory<ProductionUserSpot>(
                    "Tests.ProductionUserSpot",
                    factory => factory
                        .StableTypeLimit(1)
                        .DisableRelocation());
        });

        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(
            runtime.PrepareLocationNodeRoutingId(),
            CancellationToken.None);
        PreparedReservedSpot? prepared = null;
        ZLinkSpotNodeRuntime? node = null;
        ZLinkSpotNodeCatalog? generatedCatalog = null;
        await using var timerScheduler = new ZLinkTimerScheduler();
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            node = runtime.GetSpotNodeRuntime("objects");
            prepared = await node.Catalog.PrepareReservedAsync(
                typeof(ProductionUserSpot),
                $"prepared-{Guid.NewGuid():D}",
                objectGeneration: 1,
                authorityOwnerGeneration: 1,
                ZLinkMessage.Empty,
                CancellationToken.None);

            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                () => node.Catalog.PrepareReservedAsync(
                        typeof(ProductionUserSpot),
                        $"prepared-{Guid.NewGuid():D}",
                        objectGeneration: 2,
                        authorityOwnerGeneration: 2,
                        ZLinkMessage.Empty,
                        CancellationToken.None)
                    .AsTask());
            Assert.Equal(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                error.Kind);

            if (prepared is { } reservedPrepared)
            {
                await node.Catalog.DiscardReservedAsync(reservedPrepared);
                prepared = null;
            }
            generatedCatalog = new ZLinkSpotNodeCatalog(
                provider,
                runtime,
                runtime.Registration,
                node.Registration,
                node.Node,
                "objects",
                runtime.CompletionAdmission,
                lifecycle: null,
                timerScheduler: timerScheduler);
            var createGate = ProductionUserSpot.BlockNextCreate();
            var firstCreate = generatedCatalog.CreateAsync(
                    typeof(ProductionUserSpot),
                    ZLinkMessage.Empty,
                    CancellationToken.None)
                .AsTask();
            try
            {
                await createGate.Entered.Task.WaitAsync(
                    TimeSpan.FromSeconds(5));
                var generatedError = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                    () => generatedCatalog.CreateAsync(
                            typeof(ProductionUserSpot),
                            ZLinkMessage.Empty,
                            CancellationToken.None)
                        .AsTask());
                Assert.Equal(
                    ZLinkFrameworkErrorKind.CapacityExceeded,
                    generatedError.Kind);
            }
            finally
            {
                createGate.Release.TrySetResult();
            }
            var generated = await firstCreate;
            Assert.Equal(ZLinkSpotCreateState.Created, generated.State);
        }
        finally
        {
            if (prepared is { } reserved && node is { } spotNode)
                await spotNode.Catalog.DiscardReservedAsync(reserved);
            if (generatedCatalog is not null)
                await generatedCatalog.DisposeAsync();
            await timerScheduler.DisposeAsync();
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task InstanceSpotMonitoringIncludesPreparedActivation()
    {
        var relocationStore = new TestRelocationStore();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(relocationStore);
            var node = options.AddRouteMesh("objects")
                .Listen($"tcp://127.0.0.1:{FindFreeTcpPort()}");
            node.Objects()
                .Server()
                .AddInstanceSpotFactory<MonitoringInstanceSpot>(
                    "Tests.MonitoringInstanceSpot",
                    factory => factory
                        .StableTypeLimit(1)
                        .DisableRelocation());
        });

        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(
            runtime.PrepareLocationNodeRoutingId(),
            CancellationToken.None);
        PreparedReservedSpot? prepared = null;
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            var node = runtime.GetSpotNodeRuntime("objects");
            prepared = await node.Catalog.PrepareInstanceReservedAsync(
                "Tests.MonitoringInstanceSpot",
                $"instance-{Guid.NewGuid():D}",
                objectGeneration: 1,
                authorityOwnerGeneration: 1,
                CancellationToken.None);

            var snapshot = Assert.Single(
                node.GetInstanceSpotMonitoringSnapshots());
            Assert.Equal(0UL, snapshot.ActiveCount);
            Assert.Equal(1UL, snapshot.ActivatingCount);
            Assert.Equal(0UL, snapshot.ClosingCount);
        }
        finally
        {
            if (prepared is { } reserved)
                await runtime.GetSpotNodeRuntime("objects")
                    .Catalog.DiscardReservedAsync(reserved);
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task InstanceSpotIdleInspectionRotatesWithABoundedBatch()
    {
        const string stableType = "Tests.IdleBatchInstanceSpot";
        var relocationStore = new TestRelocationStore();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(relocationStore);
            var node = options.AddRouteMesh("objects")
                .Listen($"tcp://127.0.0.1:{FindFreeTcpPort()}")
                .SetSpotLimit(128);
            node.Objects()
                .Server()
                .AddInstanceSpotFactory<MonitoringInstanceSpot>(
                    stableType,
                    factory => factory
                        .StableTypeLimit(128)
                        .DisableRelocation());
        });

        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        await locations.StartAsync(
            runtime.PrepareLocationNodeRoutingId(),
            CancellationToken.None);
        ZLinkSpotNodeCatalog? catalog = null;
        ZLinkTimerScheduler? timerScheduler = null;
        ZLinkSpotNodeCatalog? singleCatalog = null;
        ZLinkTimerScheduler? singleTimerScheduler = null;
        try
        {
            await runtime.StartAsync(CancellationToken.None);
            var nodeRuntime = runtime.GetSpotNodeRuntime("objects");
            timerScheduler = new ZLinkTimerScheduler();
            catalog = new ZLinkSpotNodeCatalog(
                provider,
                runtime,
                runtime.Registration,
                nodeRuntime.Registration,
                nodeRuntime.Node,
                "objects",
                runtime.CompletionAdmission,
                lifecycle: null,
                timerScheduler: timerScheduler!);
            var total = ZLinkSpotNodeCatalog.IdleEvictionBatchSize + 1;

            for (var index = 0; index < total; index++)
            {
                var prepared = await catalog.PrepareInstanceReservedAsync(
                    stableType,
                    $"idle-batch-{index}-{Guid.NewGuid():N}",
                    objectGeneration: 1,
                    authorityOwnerGeneration: 1,
                    CancellationToken.None);
                await catalog.PublishInstanceReservedAsync(
                    prepared,
                    objectGeneration: 1,
                    authorityOwnerGeneration: 1,
                    CancellationToken.None);
            }

            var first = catalog.SnapshotIdleEvictionCandidates();
            var second = catalog.SnapshotIdleEvictionCandidates();
            var distinct = first
                .Concat(second)
                .Select(static activation => activation.SpotId)
                .Distinct(StringComparer.Ordinal)
                .Count();

            Assert.Equal(ZLinkSpotNodeCatalog.IdleEvictionBatchSize, first.Count);
            Assert.Equal(ZLinkSpotNodeCatalog.IdleEvictionBatchSize, second.Count);
            Assert.Equal(total, distinct);

            singleTimerScheduler = new ZLinkTimerScheduler();
            singleCatalog = new ZLinkSpotNodeCatalog(
                provider,
                runtime,
                runtime.Registration,
                nodeRuntime.Registration,
                nodeRuntime.Node,
                "objects",
                runtime.CompletionAdmission,
                lifecycle: null,
                timerScheduler: singleTimerScheduler);
            var singlePrepared = await singleCatalog.PrepareInstanceReservedAsync(
                stableType,
                $"idle-single-{Guid.NewGuid():N}",
                objectGeneration: 1,
                authorityOwnerGeneration: 1,
                CancellationToken.None);
            await singleCatalog.PublishInstanceReservedAsync(
                singlePrepared,
                objectGeneration: 1,
                authorityOwnerGeneration: 1,
                CancellationToken.None);

            var singleFirst = singleCatalog.SnapshotIdleEvictionCandidates();
            var singleSecond = singleCatalog.SnapshotIdleEvictionCandidates();
            Assert.Single(singleFirst);
            Assert.Single(singleSecond);
            Assert.Equal(singleFirst[0].SpotId, singleSecond[0].SpotId);
        }
        finally
        {
            if (singleCatalog is not null)
                await singleCatalog.DisposeAsync();
            if (singleTimerScheduler is not null)
                await singleTimerScheduler.DisposeAsync();
            if (catalog is not null)
                await catalog.DisposeAsync();
            if (timerScheduler is not null)
                await timerScheduler.DisposeAsync();
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task FrameworkHostAutomaticallyExecutesRemoteUserSpotCreateAndCloseAgainstAuthorityStore()
    {
        ProductionUserSpot.Reset();
        var suffix = Guid.NewGuid().ToString("N");
        var targetRid = RoutingId.From($"production-target-{suffix}");
        var sourceRid = RoutingId.From($"production-source-{suffix}");
        var targetEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var sourceEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        const string stableType = "Tests.ProductionUserSpot";
        var relocationStore = new TestRelocationStore();

        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.UseTestLocationStore();
            options.AddRelocationStore(relocationStore);
            var node = options.AddRouteMesh("objects")
                .Listen(targetEndpoint)
                .SetRoutingIdPrefix($"production-target-{suffix}")
                .SetSpotLimit(100);
            node.Objects().Server().AddSpotFactory<ProductionUserSpot>(
                stableType,
                factory => factory
                    .StableTypeLimit(100)
                    .DisableRelocation());
        });

        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        var autoConnect =
            provider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        targetRid = runtime.PrepareLocationNodeRoutingId();
        await locations.StartAsync(targetRid, CancellationToken.None);
        await runtime.StartAsync(CancellationToken.None);
        await autoConnect.StartAsync(
            await runtime.GetStartedStateForRoutingAsync(CancellationToken.None),
            CancellationToken.None);
        try
        {
            var target = runtime.GetSpotNodeRuntime("objects");
            Assert.NotNull(target.StartupState);
            targetRid = target.Node.RoutingId;
            await using var sourceContext = Systems.Zlink.Zlink.CreateContext();
            await using var source = new ZLinkManagedMeshNode(sourceContext, "objects");
            source.SetRoutingId(sourceRid);
            source.SetBind(sourceEndpoint);
            source.ConnectPeer(targetEndpoint, targetRid);
            target.Node.ConnectPeer(sourceRid, sourceEndpoint);
            source.Start();
            await WaitUntilAsync(() =>
                source.Status().AdmittedPeerCount == 1
                && target.Node.MeshStatus().AdmittedPeerCount == 1);

            var store = Assert.IsAssignableFrom<IZLinkLocationRepository>(
                runtime.Registration.Locations.ResolveStore());
            var owner = locations.OwnerToken;
            var targetGeneration = target.Node.MeshStatus().LifecycleGeneration;
            var descriptorKey = new ZLinkMeshNodeDescriptorKey("objects", targetRid);
            var expectedDescriptor = new ZLinkMeshNodeDescriptor(
                "objects",
                targetRid,
                targetGeneration,
                1,
                targetEndpoint,
                new Dictionary<string, int>(StringComparer.Ordinal)
                {
                    ["objects"] = 100
                },
                string.Empty,
                owner.OwnerId,
                owner.LeaseGeneration,
                DateTimeOffset.UtcNow)
            {
                ObjectRole = ZLinkMeshNodeObjectRole.Server,
                EntrySpotId = $"production-entry-{Guid.NewGuid():D}",
                State = ZLinkFrameworkRuntimeState.Serving,
                ObjectCapabilities =
                [
                    new ZLinkObjectCapability(
                        ZLinkPlacementObjectKind.UserSpot,
                        stableType,
                        ZLinkObjectMaintenancePolicyKind.Disabled,
                        false,
                        100)
                ],
                Capacity = new ZLinkPlacementCapacity(
                    new ZLinkPopulationCapacity(0, 0, 0),
                    new ZLinkPopulationCapacity(0, 0, 100),
                    [
                        new ZLinkSpotTypeCapacity(
                            ZLinkPlacementObjectKind.UserSpot,
                            stableType,
                            0,
                            0,
                            100)
                    ])
            };
            var descriptors = (await store.ListMeshNodesAsync(
                    "objects",
                    new ZLinkPageRequest(100),
                    CancellationToken.None))
                .Items;
            var descriptor = Assert.Single(
                descriptors,
                row => row.Rid == targetRid);
            Assert.Equal(targetGeneration, descriptor.LifecycleGeneration);
            Assert.Equal(expectedDescriptor.OwnerId, descriptor.OwnerId);
            Assert.Equal(expectedDescriptor.LeaseGeneration,
                descriptor.LeaseGeneration);

            var spotId = $"production-spot-{suffix}";
            var authorityKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
            var creationPayload = ZLinkApplicationPayloadEnvelopeCodec.Encode(
                ZLinkApplicationPayloadEnvelopeCodec.CreationPacketName,
                ZLinkEnvelopeCodec.DefaultContentType,
                "{}"u8);
            var creationReference =
                ZLinkInlineCreationIntentCodec.Encode(creationPayload);
            var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.UserSpot,
                        authorityKey,
                        stableType,
                        creationReference,
                        System.Security.Cryptography.SHA256.HashData(creationPayload),
                        creationPayload.Length,
                        descriptorKey,
                        targetGeneration,
                        owner,
                        ZLinkUserSpotAuthorityPayloadCodec.Encode(
                            new ZLinkUserSpotAuthorityPayload(
                                ZLinkUserSpotAuthorityState.Creating,
                                stableType,
                                spotId,
                                owner.OwnerId,
                                checked((ulong)owner.LeaseGeneration),
                                "objects",
                                targetRid,
                                targetGeneration)),
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.UserSpot,
                                stableType,
                                1)))));
            var fence = new ObjectReservationFence(
                reservation.Reservation.ReservationVersion,
                reservation.Reservation.StoreVersion,
                reservation.Reservation.ObjectGeneration,
                reservation.Reservation.AuthorityOwnerGeneration,
                targetRid,
                targetGeneration,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                1);
            var deadline = checked(
                (ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds());

            Assert.Equal(
                SubmitResult.Ok,
                source.CreateUserSpot(
                    targetRid,
                    spotId,
                    stableType,
                    fence,
                    deadline,
                    out var createOperation,
                    TimeSpan.FromSeconds(3)));
            await WaitUntilAsync(() =>
                source.Status().PendingInfrastructureMessages > 0);
            var (createCompletion, replyParts) = DrainCompletion(
                source,
                createOperation);
            try
            {
                Assert.Equal((int)RequestResult.Ok, createCompletion.TerminalResult);
                Assert.Equal(
                    UserSpotCreateResult.Created,
                    createCompletion.UserSpotCreateCompletion?.Result);
                Assert.Equal(1, ProductionUserSpot.CreateCount);
                Assert.Equal(
                    reservation.Reservation.ObjectGeneration,
                    ProductionUserSpot.ObjectGenerationObservedDuringCreate);
                Assert.Equal(2, replyParts.Count);
                var replyHeader = ZLinkEnvelopeCodec.DecodeHeader(replyParts);
                Assert.Equal(ZLinkMessageKind.Response, replyHeader.Kind);
                Assert.Equal(string.Empty, replyHeader.MessageName);
                Assert.Contains(
                    "production-created",
                    System.Text.Encoding.UTF8.GetString(
                        replyParts[1].AsReadOnlySpan()),
                    StringComparison.Ordinal);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(replyParts);
            }

            var active = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(authorityKey));
            var closeFence = new UserSpotCloseFence(
                spotId,
                active.Snapshot.ObjectGeneration,
                targetRid,
                targetGeneration,
                active.Snapshot.AuthorityOwnerGeneration,
                active.Snapshot.StoreVersion);
            Assert.Equal(
                SubmitResult.Ok,
                source.CloseUserSpot(
                    targetRid,
                    closeFence,
                    deadline,
                    out var closeOperation,
                    TimeSpan.FromSeconds(3)));
            await WaitUntilAsync(() =>
                source.Status().PendingInfrastructureMessages > 0);
            var (closeCompletion, closeParts) = DrainCompletion(
                source,
                closeOperation);
            ZLinkMessageParts.DisposeAll(closeParts);
            Assert.Equal(
                new UserSpotCloseCompletion(true),
                closeCompletion.UserSpotCloseCompletion);
            Assert.Equal(1, ProductionUserSpot.CloseCount);
            Assert.Equal(
                ZLinkSpotCloseReason.ExplicitClose,
                ProductionUserSpot.LastClosingContext?.Reason);
            Assert.True(
                ProductionUserSpot.LastClosingContext?.Deadline
                > DateTimeOffset.UtcNow);
            Assert.False(ProductionUserSpot.CleanupTokenWasCanceledAtInvocation);
            Assert.IsType<ZLinkAuthorityReadResult.Missing>(
                await store.ReadAuthorityAsync(authorityKey));

            var orphanRid = $"production-orphan-{suffix}";
            var orphanKey = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(orphanRid);
            var orphanReservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.UserSpot,
                        orphanKey,
                        stableType,
                        creationReference,
                        System.Security.Cryptography.SHA256.HashData(creationPayload),
                        creationPayload.Length,
                        descriptorKey,
                        targetGeneration,
                        owner,
                        ZLinkUserSpotAuthorityPayloadCodec.Encode(
                            new ZLinkUserSpotAuthorityPayload(
                                ZLinkUserSpotAuthorityState.Creating,
                                stableType,
                                orphanRid,
                                owner.OwnerId,
                                checked((ulong)owner.LeaseGeneration),
                                "objects",
                                targetRid,
                                targetGeneration)),
                        new ZLinkCapacityVector(
                            0,
                            1,
                            new ZLinkSpotTypeCapacityDelta(
                                ZLinkPlacementObjectKind.UserSpot,
                                stableType,
                                1)))));
            var orphanActive = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await store.CommitAsync(
                    orphanReservation.Reservation,
                    ZLinkUserSpotAuthorityPayloadCodec.Encode(
                        new ZLinkUserSpotAuthorityPayload(
                            ZLinkUserSpotAuthorityState.Ready,
                            stableType,
                            orphanRid,
                            owner.OwnerId,
                            checked((ulong)owner.LeaseGeneration),
                            "objects",
                            targetRid,
                            targetGeneration))));
            var orphanFence = new UserSpotCloseFence(
                orphanRid,
                orphanActive.Snapshot.ObjectGeneration,
                targetRid,
                targetGeneration,
                orphanActive.Snapshot.AuthorityOwnerGeneration,
                orphanActive.Snapshot.StoreVersion);
            Assert.Equal(
                SubmitResult.Ok,
                source.CloseUserSpot(
                    targetRid,
                    orphanFence,
                    checked((ulong)DateTimeOffset.UtcNow.AddSeconds(5)
                        .ToUnixTimeMilliseconds()),
                    out var orphanCloseOperation,
                    TimeSpan.FromSeconds(3)));
            await WaitUntilAsync(() =>
                source.Status().PendingInfrastructureMessages > 0);
            var (orphanCompletion, orphanParts) = DrainCompletion(
                source,
                orphanCloseOperation);
            ZLinkMessageParts.DisposeAll(orphanParts);
            Assert.Equal(
                (int)RequestResult.Conflict,
                orphanCompletion.TerminalResult);
            Assert.Equal(
                (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving,
                orphanCompletion.FailureErrno);
            Assert.Null(orphanCompletion.UserSpotCloseCompletion);
            var retainedOrphan = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(orphanKey));
            Assert.True(ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                retainedOrphan.Snapshot.Payload.Span,
                out var retainedPayload));
            Assert.Equal(
                ZLinkUserSpotAuthorityState.Ready,
                retainedPayload.State);
        }
        finally
        {
            await autoConnect.StopAsync(CancellationToken.None);
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task PublicSpotManagerUsesReserveAndRemoteUserSpotCommands()
    {
        ProductionUserSpot.Reset();
        var suffix = Guid.NewGuid().ToString("N");
        var locationStore = new ZLinkInMemoryLocationStore();
        var sourceRid = RoutingId.From($"public-source-{suffix}");
        var targetRid = RoutingId.From($"public-target-{suffix}");
        var sourceEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var targetEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";

        ServiceProvider Build(
            RoutingId rid,
            string endpoint,
            bool server)
        {
            var services = new ServiceCollection();
            services.AddZLinkFramework(options =>
            {
                options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
                options.AddLocationStore(locationStore);
                var node = options.AddRouteMesh("objects")
                    .Listen(endpoint)
                    .SetRoutingIdPrefix(rid.ToString())
                    .SetSpotLimit(100);
                var objects = node.Objects();
                if (server)
                {
                    objects.Server()
                        .AddSpotFactory<ProductionUserSpot>(
                            "Tests.ProductionUserSpot",
                            factory => factory
                                .StableTypeLimit(100)
                                .DisableRelocation());
                }
                else
                {
                    objects.Client();
                }
            });
            return services.BuildServiceProvider();
        }

        await using var targetProvider = Build(targetRid, targetEndpoint, true);
        await using var sourceProvider = Build(
            sourceRid,
            sourceEndpoint,
            false);
        var target = targetProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var source = sourceProvider.GetRequiredService<ZLinkFrameworkRuntime>();
        var targetLocations = targetProvider.GetRequiredService<ZLinkLocationRuntime>();
        var sourceLocations = sourceProvider.GetRequiredService<ZLinkLocationRuntime>();
        var targetAutoConnect =
            targetProvider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var sourceAutoConnect =
            sourceProvider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        targetRid = target.PrepareLocationNodeRoutingId();
        sourceRid = source.PrepareLocationNodeRoutingId();
        await targetLocations.StartAsync(targetRid, CancellationToken.None);
        await sourceLocations.StartAsync(sourceRid, CancellationToken.None);
        await target.StartAsync(CancellationToken.None);
        await source.StartAsync(CancellationToken.None);
        targetRid = target.GetSpotNodeRuntime("objects").Node.RoutingId;
        sourceRid = source.GetSpotNodeRuntime("objects").Node.RoutingId;
        await targetAutoConnect.StartAsync(
            await target.GetStartedStateForRoutingAsync(CancellationToken.None),
            CancellationToken.None);
        await sourceAutoConnect.StartAsync(
            await source.GetStartedStateForRoutingAsync(CancellationToken.None),
            CancellationToken.None);
        try
        {
            target.GetSpotNodeRuntime("objects").Node.ConnectPeer(
                sourceRid,
                sourceEndpoint);
            await WaitUntilAsync(() =>
                source.GetSpotNodeRuntime("objects").Node.MeshStatus()
                    .AdmittedPeerCount > 0);
            ZLinkMeshNodeDescriptor? currentDescriptor = null;
            for (var attempt = 0; attempt < 500 && currentDescriptor is null; attempt++)
            {
                currentDescriptor = (await locationStore.ListMeshNodesAsync("objects", default)).Items
                    .SingleOrDefault(row => row.Rid == targetRid);
                if (currentDescriptor is null)
                    await Task.Delay(10);
            }
            var descriptor = Assert.IsType<ZLinkMeshNodeDescriptor>(currentDescriptor);
            Assert.Contains(
                descriptor.ObjectCapabilities,
                capability =>
                    capability.ObjectKind == ZLinkPlacementObjectKind.UserSpot
                    && capability.StableType == "Tests.ProductionUserSpot");
            var spotId = $"public-spot-{suffix}";
            using var operationTimeout = new CancellationTokenSource(
                TimeSpan.FromSeconds(10));
            using var customPayload = Message.From([9, 8, 7]);
            var allocated = await source
                .Create("Tests.ProductionUserSpot")
                .Request(ZLinkMessage.FromEnvelopePayload(
                    "application/x-zlink-test",
                    customPayload,
                    source.Registration.Codecs))
                .Async(operationTimeout.Token);
            Assert.Equal(ZLinkSpotCreateState.Created, allocated.State);
            Assert.Equal(
                "application/x-zlink-test",
                ProductionUserSpot.LastCreateContentType);
            Assert.Equal(allocated.Spot, await source.FindAsync(allocated.Spot.SpotId));
            Assert.NotNull(await target.FindAsync(allocated.Spot.SpotId));
            Assert.True(await source.CloseAsync(
                allocated.Spot,
                operationTimeout.Token));
            Assert.Null(await target.FindAsync(allocated.Spot.SpotId));

            var created = await source
                .GetOrCreate(spotId, "Tests.ProductionUserSpot")
                .Request(new { Name = "created" })
                .Async(operationTimeout.Token);
            Assert.Equal(ZLinkSpotCreateState.Created, created.State);
            Assert.Equal(created.Spot, await source.FindAsync(spotId));
            Assert.NotNull(await target.FindAsync(spotId));
            var existing = await source
                .GetOrCreate(spotId, "Tests.ProductionUserSpot")
                .Async(operationTimeout.Token);
            Assert.Equal(ZLinkSpotCreateState.Existing, existing.State);
            Assert.Equal(created.Spot, existing.Spot);
            Assert.True(await source.CloseAsync(
                created.Spot,
                operationTimeout.Token));
            Assert.Null(await target.FindAsync(spotId));

            var joiningRid = $"joining-spot-{suffix}";
            var createGate = ProductionUserSpot.BlockNextCreate();
            var ownerCreate = source
                .GetOrCreate(joiningRid, "Tests.ProductionUserSpot")
                .Async(operationTimeout.Token)
                .AsTask();
            await createGate.Entered.Task.WaitAsync(operationTimeout.Token);
            var joiningPending = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await locationStore.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(joiningRid),
                    operationTimeout.Token));
            Assert.Equal(
                ZLinkPlacementAllocationState.Reserved,
                joiningPending.Snapshot.Allocation.State);
            Assert.StartsWith(
                "inline-v1:",
                joiningPending.Snapshot.ReservedCreation!
                    .RequestContentReference,
                StringComparison.Ordinal);
            var joinedCreate = source
                .GetOrCreate(joiningRid, "Tests.ProductionUserSpot")
                .Async(operationTimeout.Token)
                .AsTask();
            Assert.False(joinedCreate.IsCompleted);
            createGate.Release.TrySetResult();
            var ownerResult = await ownerCreate;
            var joinedResult = await joinedCreate;
            Assert.Equal(ZLinkSpotCreateState.Created, ownerResult.State);
            Assert.Equal(ZLinkSpotCreateState.Existing, joinedResult.State);
            Assert.Equal(ownerResult.Spot, joinedResult.Spot);
            Assert.True(await source.CloseAsync(
                ownerResult.Spot,
                operationTimeout.Token));
        }
        finally
        {
            await sourceAutoConnect.StopAsync(CancellationToken.None);
            await source.StopAsync(CancellationToken.None);
            await sourceLocations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await sourceLocations.StopAsync(CancellationToken.None);
            await targetAutoConnect.StopAsync(CancellationToken.None);
            await target.StopAsync(CancellationToken.None);
            await targetLocations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await targetLocations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteUserSpotCreateAndCloseUseGenerationFencedTerminalOperations()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "user-spot-source");
        await using var target = NewNode(context, "user-spot-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://user-spot-source-{suffix}";
        var targetEndpoint = $"inproc://user-spot-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        var operationTarget = new RecordingUserSpotOperationTarget();
        target.SetUserSpotOperationTarget(operationTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        var sourceGeneration = source.Status().LifecycleGeneration;
        var targetGeneration = target.Status().LifecycleGeneration;
        const string spotId = "remote-created-spot";
        var reservation = new ObjectReservationFence(
            "reservation-runtime",
            "store-runtime-1",
            71,
            73,
            target.RoutingId,
            targetGeneration,
            "target-owner",
            79,
            1);
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds());

        Assert.Equal(
            SubmitResult.Ok,
            source.CreateUserSpot(
                target.RoutingId,
                spotId,
                "Sample.RemoteSpot",
                reservation,
                deadline,
                out var createOperation,
                TimeSpan.FromSeconds(3)));
        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var createRecords = DrainRecords(source);
        var createCompletion = Assert.Single(createRecords.Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == createOperation));
        Assert.Equal(MeshOperationKind.UserSpotCreate, createCompletion.OperationKind);
        Assert.Equal((int)RequestResult.Ok, createCompletion.TerminalResult);
        Assert.Equal(
            new UserSpotCreateCompletion(
                UserSpotCreateResult.Created,
                spotId,
                reservation.ObjectGeneration),
            createCompletion.UserSpotCreateCompletion);
        Assert.Equal(1, createCompletion.PartCount);
        Assert.Equal(1, operationTarget.CreateCount);
        Assert.Equal(source.RoutingId, operationTarget.LastCreate.SourceNodeRid);
        Assert.Equal(sourceGeneration, operationTarget.LastCreate.SourceNodeGeneration);
        Assert.Equal(reservation, operationTarget.LastCreate.Reservation);

        var closeFence = new UserSpotCloseFence(
            spotId,
            reservation.ObjectGeneration,
            target.RoutingId,
            targetGeneration,
            reservation.AuthorityOwnerGeneration,
            "store-runtime-2");
        Assert.Equal(
            SubmitResult.Ok,
            source.CloseUserSpot(
                target.RoutingId,
                closeFence,
                deadline,
                out var closeOperation,
                TimeSpan.FromSeconds(3)));
        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var closeRecords = DrainRecords(source);
        var closeCompletion = Assert.Single(closeRecords.Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == closeOperation));
        Assert.Equal(MeshOperationKind.UserSpotClose, closeCompletion.OperationKind);
        Assert.Equal((int)RequestResult.Ok, closeCompletion.TerminalResult);
        Assert.Equal(new UserSpotCloseCompletion(true), closeCompletion.UserSpotCloseCompletion);
        Assert.Equal(1, operationTarget.CloseCount);
        Assert.Equal(closeFence, operationTarget.LastClose.Target);

        operationTarget.CloseError = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "The User Spot is sealed for relocation.",
            ZLinkRetryAdvice.RetryAfterBackoff);
        Assert.Equal(
            SubmitResult.Ok,
            source.CloseUserSpot(
                target.RoutingId,
                closeFence,
                deadline,
                out var movingOperation,
                TimeSpan.FromSeconds(3)));
        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var movingRecords = DrainRecords(source);
        var movingCompletion = Assert.Single(movingRecords.Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == movingOperation));
        Assert.Equal((int)RequestResult.Conflict, movingCompletion.TerminalResult);
        Assert.Equal(
            (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving,
            movingCompletion.FailureErrno);
        Assert.Null(movingCompletion.UserSpotCloseCompletion);
        Assert.Equal(2, operationTarget.CloseCount);

        Assert.Equal(
            SubmitResult.NotConnected,
            source.CloseUserSpot(
                target.RoutingId,
                closeFence with { TargetNodeGeneration = targetGeneration + 1 },
                deadline,
                out var staleOperation,
                TimeSpan.FromSeconds(1)));
        Assert.Equal(default, staleOperation);
        Assert.Equal(2, operationTarget.CloseCount);
    }

    [Fact]
    public async Task RelocationReplyRelayUsesRawCommandsAndRetriesAfterAckLoss()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "relay-source");
        await using var target = NewNode(context, "relay-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://relay-source-{suffix}";
        var targetEndpoint = $"inproc://relay-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(sourceEndpoint, source.RoutingId);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "request-owner",
            17,
            target.RoutingId,
            target.Status().LifecycleGeneration);
        var relayTarget = new RecordingReplyRelayTarget(
            requestSource,
            dropFirstAcknowledgement: true,
            corruptSecondSource: true,
            corruptThirdReplyRoute: true);
        target.SetRelocationReplyRelayTarget(relayTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 1
                                  && target.Status().AdmittedPeerCount == 1);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(21, 22),
            23,
            new ZLinkServiceWireCodec.RelocationWireId(24, 25),
            source.Status().LifecycleGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator",
                26,
                source.RoutingId,
                source.Status().LifecycleGeneration,
                "store-27"),
            28,
            29,
            0,
            ServiceWireConstants.FrameworkErrorCode.None);
        var payload = new[] { Message.From(new byte[] { 7, 8, 9 }) };
        try
        {
            await Assert.ThrowsAsync<TimeoutException>(async () =>
                await source.RelayRelocationReplyAsync(
                    target.RoutingId,
                    relay,
                    requestSource,
                    payload,
                    TimeSpan.FromMilliseconds(50),
                    CancellationToken.None));
            await Assert.ThrowsAsync<TimeoutException>(async () =>
                await source.RelayRelocationReplyAsync(
                    target.RoutingId,
                    relay,
                    requestSource,
                    payload,
                    TimeSpan.FromMilliseconds(50),
                    CancellationToken.None));
            await Assert.ThrowsAsync<TimeoutException>(async () =>
                await source.RelayRelocationReplyAsync(
                    target.RoutingId,
                    relay,
                    requestSource,
                    payload,
                    TimeSpan.FromMilliseconds(50),
                    CancellationToken.None));
            var ack = await source.RelayRelocationReplyAsync(
                target.RoutingId,
                relay,
                requestSource,
                payload,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);

            Assert.Equal((byte)2, ack.Status);
            Assert.Equal(requestSource, ack.RequestSource);
            Assert.Equal(4, relayTarget.InvocationCount);
            Assert.All(relayTarget.Payloads, bytes =>
                Assert.Equal(new byte[] { 7, 8, 9 }, bytes));
            await Assert.ThrowsAsync<ArgumentException>(async () =>
                await source.RelayRelocationReplyAsync(
                    target.RoutingId,
                    relay with
                    {
                        TerminalResult = 105,
                        FailureCode = ServiceWireConstants.FrameworkErrorCode
                            .RequestFailed
                    },
                    requestSource,
                    payload,
                    TimeSpan.FromSeconds(1),
                    CancellationToken.None));
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(payload);
        }
    }

    [Fact]
    public void RelocationReplyPendingKeysSeparateTheSameOperationAcrossSources()
    {
        var sourceA = RoutingId.From("relay-source-a");
        var sourceB = RoutingId.From("relay-source-b");
        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            new MeshOperationId(41, 42),
            43,
            new ZLinkServiceWireCodec.RelocationWireId(44, 45),
            46,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator",
                47,
                RoutingId.From("relay-target"),
                48,
                "store-49"),
            48,
            49,
            0,
            ServiceWireConstants.FrameworkErrorCode.None);
        var keyA = ZLinkManagedMeshNode.PendingReplyRelayKey.Create(
            sourceA,
            relay);
        var keyB = ZLinkManagedMeshNode.PendingReplyRelayKey.Create(
            sourceB,
            relay);

        Assert.NotEqual(keyA, keyB);
        Assert.Equal(
            keyA,
            ZLinkManagedMeshNode.PendingReplyRelayKey.Create(
                sourceA,
                new ZLinkServiceWireCodec.ReplyRelayAckRecord(
                    relay.RelocationId,
                    relay.Coordinator,
                    relay.OperationId,
                    relay.ReplyRouteId,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        "owner-a", 50, sourceA, 51),
                    1)));
        Assert.NotEqual(
            keyA,
            ZLinkManagedMeshNode.PendingReplyRelayKey.Create(
                sourceA,
                new ZLinkServiceWireCodec.ReplyRelayAckRecord(
                    relay.RelocationId,
                    relay.Coordinator,
                    relay.OperationId,
                    relay.ReplyRouteId + 1,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        "owner-a", 50, sourceA, 51),
                    1)));
        var exactSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "owner-a", 50, sourceA, 51);
        Assert.True(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            sourceA, 51, exactSource, exactSource));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            sourceA, 51, exactSource, exactSource with { OwnerId = "owner-b" }));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            sourceA, 51, exactSource, exactSource with { LeaseGeneration = 52 }));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            sourceA, 51, exactSource, exactSource with { NodeRid = sourceB }));
        Assert.False(ZLinkManagedMeshNode.IsExactReplyRelayAckSource(
            sourceA, 51, exactSource, exactSource with { NodeGeneration = 52 }));
        Assert.True(ZLinkManagedMeshNode.IsReplyRelayPayloadAllowed(0, 1));
        Assert.True(ZLinkManagedMeshNode.IsReplyRelayPayloadAllowed(105, 0));
        Assert.False(ZLinkManagedMeshNode.IsReplyRelayPayloadAllowed(105, 1));
        Assert.False(ZLinkManagedMeshNode.IsReplyRelayPayloadAllowed(0, 2));
    }

    [Fact]
    public async Task RemoteInstanceSpotColdActivationDispatchesFirstMessageThroughCommand39()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "instance-source");
        await using var target = NewNode(context, "instance-target");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://instance-source-{suffix}";
        var targetEndpoint = $"inproc://instance-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        var activationTarget = new RecordingInstanceSpotActivationTarget();
        target.SetInstanceSpotActivationTarget(activationTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        using var firstMessage = Message.From([1, 2, 3]);
        var activation = new InstanceSpotActivationTarget(
            "objects",
            target.RoutingId,
            target.Status().LifecycleGeneration,
            "cold-instance",
            "Sample.InstanceSpot",
            "descriptor-1");
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds());

        Assert.Equal(
            SubmitResult.Ok,
            source.ActivateInstanceSpot(
                activation,
                "caller-spot",
                [firstMessage],
                request: true,
                out var operationId,
                deadline,
                TimeSpan.FromSeconds(3),
                metadata: new byte[] { 9, 8 }));

        await WaitUntilAsync(() =>
            source.Status().PendingInfrastructureMessages > 0);
        var completion = DrainCompletion(source, operationId);
        try
        {
            Assert.Equal(MeshOperationKind.InstanceSpotRequest, completion.Record.OperationKind);
            Assert.Equal((int)RequestResult.Ok, completion.Record.TerminalResult);
            Assert.Equal([7, 6], completion.Parts.Single().ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(completion.Parts);
        }

        Assert.Equal(1, activationTarget.Count);
        Assert.Equal(activation, activationTarget.LastOperation.Target);
        Assert.Equal(operationId, activationTarget.LastOperation.OperationId);
        Assert.Equal(operationId.Low, activationTarget.LastOperation.ReplyRouteId);
        Assert.Equal(source.RoutingId, activationTarget.LastOperation.SourceNodeRid);
        Assert.Equal([9, 8], activationTarget.LastMetadata.ToArray());
        Assert.Equal([1, 2, 3], activationTarget.LastPayload.Single().ToArray());
    }

    [Fact]
    public async Task InstanceSpotActivationLoserForwardsOriginalOperationToWinner()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "instance-forward-source");
        await using var loser = NewNode(context, "instance-forward-loser");
        await using var winner = NewNode(context, "instance-forward-winner");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://instance-forward-source-{suffix}";
        var loserEndpoint = $"inproc://instance-forward-loser-{suffix}";
        var winnerEndpoint = $"inproc://instance-forward-winner-{suffix}";
        source.SetBind(sourceEndpoint);
        loser.SetBind(loserEndpoint);
        winner.SetBind(winnerEndpoint);
        source.ConnectPeer(loserEndpoint, loser.RoutingId);
        source.ConnectPeer(winnerEndpoint, winner.RoutingId);
        loser.ConnectPeer(winnerEndpoint, winner.RoutingId);
        var winnerTarget = new RecordingInstanceSpotActivationTarget();
        winner.SetInstanceSpotActivationTarget(winnerTarget);
        loser.SetInstanceSpotActivationTarget(
            new ForwardingInstanceSpotActivationTarget(loser, winner));
        source.Start();
        loser.Start();
        winner.Start();
        await WaitUntilAsync(() => source.Status().AdmittedPeerCount == 2
                                  && loser.Status().AdmittedPeerCount == 2
                                  && winner.Status().AdmittedPeerCount == 2);

        using var firstMessage = Message.From([4, 5, 6]);
        var activation = new InstanceSpotActivationTarget(
            "objects",
            loser.RoutingId,
            loser.Status().LifecycleGeneration,
            "forwarded-instance",
            "Sample.InstanceSpot",
            "descriptor-loser");
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds());
        Assert.Equal(
            SubmitResult.Ok,
            source.ActivateInstanceSpot(
                activation,
                "caller-spot",
                [firstMessage],
                request: true,
                out var operationId,
                deadline,
                TimeSpan.FromSeconds(3),
                metadata: new byte[] { 7, 8 }));

        await WaitUntilAsync(() => source.Status().PendingInfrastructureMessages > 0);
        var completion = DrainCompletion(source, operationId);
        try
        {
            Assert.Equal((int)RequestResult.Ok, completion.Record.TerminalResult);
            Assert.Equal([7, 6], completion.Parts.Single().ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(completion.Parts);
        }

        Assert.Equal(1, winnerTarget.Count);
        Assert.Equal(operationId, winnerTarget.LastOperation.OperationId);
        Assert.Equal(operationId.Low, winnerTarget.LastOperation.ReplyRouteId);
        Assert.Equal(source.RoutingId, winnerTarget.LastOperation.SourceNodeRid);
        Assert.Equal("descriptor-winner", winnerTarget.LastOperation.Target.DescriptorVersion);
        Assert.Equal([7, 8], winnerTarget.LastMetadata.ToArray());
        Assert.Equal([4, 5, 6], winnerTarget.LastPayload.Single().ToArray());
    }

    [Fact]
    public async Task RelocatedInstanceSpotReplyCompletesTheOriginalRemoteCallerExactlyOnce()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        caller.SetRoutingId(RoutingId.From("relocated-instance-caller"));
        await using var oldOwner = NewNode(
            context,
            "relocated-instance-old-owner");
        await using var target = NewNode(context, "relocated-instance-target");
        caller.SetLocalOwnerLeaseGeneration(179);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://relocated-instance-caller-{suffix}";
        var oldOwnerEndpoint =
            $"inproc://relocated-instance-old-owner-{suffix}";
        var targetEndpoint = $"inproc://relocated-instance-target-{suffix}";
        caller.SetBind(callerEndpoint);
        oldOwner.SetBind(oldOwnerEndpoint);
        target.SetBind(targetEndpoint);
        caller.ConnectPeer(oldOwnerEndpoint, oldOwner.RoutingId);
        oldOwner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(callerEndpoint, caller.RoutingId);
        var activationTarget = new BlockingInstanceSpotActivationTarget();
        oldOwner.SetInstanceSpotActivationTarget(activationTarget);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "relocated-instance-caller-owner",
            179,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        caller.SetRelocationReplyRelayTarget(
            new ZLinkRelocationReplyTarget(
                new ZLinkBackendSpotNodeWrapper(caller)));
        caller.Start();
        oldOwner.Start();
        target.Start();
        await WaitUntilAsync(() => caller.Status().AdmittedPeerCount == 2
                                  && oldOwner.Status().AdmittedPeerCount == 1
                                  && target.Status().AdmittedPeerCount == 1);

        var activation = new InstanceSpotActivationTarget(
            "objects",
            oldOwner.RoutingId,
            oldOwner.Status().LifecycleGeneration,
            "relocated-instance",
            "Tests.RelocatedInstanceSpot",
            "descriptor-instance");
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddSeconds(10).ToUnixTimeMilliseconds());
        using var request = Message.From(new byte[] { 181 });
        Assert.Equal(
            SubmitResult.Ok,
            caller.ActivateInstanceSpot(
                activation,
                "caller-entry",
                [request],
                request: true,
                out var operation,
                deadline,
                TimeSpan.FromSeconds(3)));
        Assert.Equal(
            SubmitResult.Backpressured,
            caller.ActivateInstanceSpot(
                activation with { TargetSpotId = "rejected-instance" },
                "caller-entry",
                [request],
                request: true,
                out var rejected,
                deadline,
                TimeSpan.FromSeconds(3)));
        Assert.Equal(default, rejected);
        await activationTarget.Started.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.Equal(1, activationTarget.Count);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            operation,
            operation.Low,
            new ZLinkServiceWireCodec.RelocationWireId(191, 193),
            target.Status().LifecycleGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "relocated-instance-target-owner",
                197,
                target.RoutingId,
                target.Status().LifecycleGeneration,
                "relocated-instance-root"),
            199,
            211,
            (uint)RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None);
        var response = new[] { Message.From(new byte[] { 223 }) };
        try
        {
            var firstAck = await target.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.TerminalReceived,
                firstAck.Status);
            Assert.Equal(requestSource, firstAck.RequestSource);

            await WaitUntilAsync(() =>
                caller.Status().PendingInfrastructureMessages > 0);
            var completion = Assert.Single(DrainRecords(caller).Where(record =>
                record.Kind == MeshRecordKind.Completion
                && record.OperationId == operation));
            Assert.Equal(
                MeshOperationKind.InstanceSpotRequest,
                completion.OperationKind);

            var duplicateAck = await target.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                response,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
            Assert.Equal(
                (byte)ZLinkRelocationReplyCompletionState.AlreadyTerminal,
                duplicateAck.Status);
            activationTarget.Complete();
            await Task.Delay(50);
            Assert.DoesNotContain(
                DrainRecords(caller),
                record => record.Kind == MeshRecordKind.Completion
                          && record.OperationId == operation);
            Assert.Equal(
                SubmitResult.Ok,
                caller.ActivateInstanceSpot(
                    activation with { TargetSpotId = "next-instance" },
                    "caller-entry",
                    [request],
                    request: true,
                    out var nextOperation,
                    deadline,
                    TimeSpan.FromSeconds(3)));
            Assert.NotEqual(default, nextOperation);
        }
        finally
        {
            activationTarget.Complete();
            ZLinkMessageParts.DisposeAll(response);
        }
    }

    [Fact]
    public async Task RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "retention-source");
        await using var target = NewNode(
            context,
            "retention-target",
            TimeSpan.FromMilliseconds(500));
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://retention-source-{suffix}";
        var targetEndpoint = $"inproc://retention-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(sourceEndpoint, source.RoutingId);
        var operationTarget = new RecordingUserSpotOperationTarget();
        target.SetUserSpotOperationTarget(operationTarget);
        source.Start();
        target.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        var targetGeneration = target.Status().LifecycleGeneration;
        var reservation = new ObjectReservationFence(
            "retention-reservation",
            "retention-store",
            101,
            103,
            target.RoutingId,
            targetGeneration,
            "retention-owner",
            107,
            1);
        var deadline = checked(
            (ulong)DateTimeOffset.UtcNow.AddMilliseconds(500)
                .ToUnixTimeMilliseconds());
        const string spotId = "retention-spot";
        Assert.Equal(
            SubmitResult.Ok,
            source.CreateUserSpot(
                target.RoutingId,
                spotId,
                "Sample.RetentionSpot",
                reservation,
                deadline,
                out var operationId,
                TimeSpan.FromSeconds(2)));
        await WaitUntilAsync(() =>
            source.Status().PendingInfrastructureMessages > 0);
        _ = DrainRecords(source);
        Assert.Equal(1, operationTarget.CreateCount);
        Assert.Equal(1, target.RetainedUserSpotOperationCount);

        var replay = new ZLinkServiceWireCodec.UserSpotOperationRecord(
            ServiceWireConstants.Command.UserSpotCreate,
            new UserSpotCreateOperation(
                999,
                operationId,
                source.RoutingId,
                source.Status().LifecycleGeneration,
                spotId,
                "Sample.RetentionSpot",
                reservation,
                deadline),
            default);
        var afterDeadline = DateTimeOffset.FromUnixTimeMilliseconds(
            checked((long)deadline)).AddMilliseconds(25);
        var wait = afterDeadline - DateTimeOffset.UtcNow;
        if (wait > TimeSpan.Zero)
            await Task.Delay(wait);
        Assert.Equal(
            SubmitResult.Ok,
            source.ResubmitUserSpotOperation(target.RoutingId, replay));
        await Task.Delay(50);
        Assert.Equal(1, operationTarget.CreateCount);
        Assert.Equal(1, target.RetainedUserSpotOperationCount);

        await WaitUntilAsync(() => target.RetainedUserSpotOperationCount == 0);
        Assert.Equal(
            SubmitResult.Ok,
            source.ResubmitUserSpotOperation(target.RoutingId, replay));
        await Task.Delay(50);
        Assert.Equal(1, operationTarget.CreateCount);
        Assert.Equal(0, target.RetainedUserSpotOperationCount);
    }

    private static ZLinkManagedMeshNode NewNode(
        IContext context,
        string rid,
        TimeSpan? remoteUserSpotTerminalRetention = null)
    {
        var node = new ZLinkManagedMeshNode(
            context,
            "mesh",
            remoteUserSpotTerminalRetention:
                remoteUserSpotTerminalRetention);
        node.SetRoutingId(RoutingId.From(rid));
        return node;
    }

    private sealed class RecordingReplyRelayTarget(
        ZLinkServiceWireCodec.RequestSourceFence requestSource,
        bool dropFirstAcknowledgement,
        bool corruptSecondSource = false,
        bool corruptThirdReplyRoute = false) : IRelocationReplyRelayTarget
    {
        internal int InvocationCount { get; private set; }
        internal List<byte[]> Payloads { get; } = [];

        public ValueTask<ZLinkServiceWireCodec.ReplyRelayAckRecord?> RelayAsync(
            ZLinkServiceWireCodec.ReplyRelayRecord relay,
            RoutingId sourceNodeRid,
            ulong sourceNodeGeneration,
            IReadOnlyList<Message> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            InvocationCount++;
            Payloads.Add(Assert.Single(payload).ToArray());
            ZLinkMessageParts.DisposeAll(payload);
            if (dropFirstAcknowledgement && InvocationCount == 1)
                return ValueTask.FromResult<
                    ZLinkServiceWireCodec.ReplyRelayAckRecord?>(null);
            var acknowledgedSource = corruptSecondSource && InvocationCount == 2
                ? requestSource with { OwnerId = "wrong-owner" }
                : requestSource;
            return ValueTask.FromResult<
                ZLinkServiceWireCodec.ReplyRelayAckRecord?>(
                new ZLinkServiceWireCodec.ReplyRelayAckRecord(
                    relay.RelocationId,
                    relay.Coordinator,
                    relay.OperationId,
                    corruptThirdReplyRoute && InvocationCount == 3
                        ? relay.ReplyRouteId + 1
                        : relay.ReplyRouteId,
                    acknowledgedSource,
                    2));
        }
    }

    private static List<MeshReceiveRecord> DrainRecords(ZLinkManagedMeshNode node)
    {
        var records = new List<MeshReceiveRecord>();
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
            {
                for (var record = 0; record < received.Count; record++)
                    records.Add(received[record]);
                received.Reset();
            }
        }
        return records;
    }

    private static (MeshReceiveRecord Record, IReadOnlyList<Message> Parts)
        DrainCompletion(
            ZLinkManagedMeshNode node,
            MeshOperationId operationId)
    {
        var deadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        while (true)
        {
            using var ready = new MeshReadyBatch();
            node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
            for (var index = 0; index < ready.Count; index++)
            {
                using var claim = ready.TakeClaim(index);
                using var received = new MeshReceiveBatch();
                while (claim.Receive(received, RecvFlags.DontWait))
                {
                    for (var record = 0; record < received.Count; record++)
                    {
                        var value = received[record];
                        if (value.Kind != MeshRecordKind.Completion
                            || value.OperationId != operationId)
                            continue;
                        return (value, received.RetainMessage(record));
                    }
                    received.Reset();
                }
            }

            if (Stopwatch.GetTimestamp() >= deadline)
                throw new InvalidOperationException(
                    $"Completion '{operationId}' was not queued.");
            Thread.Sleep(10);
        }
    }

    private static void DrainAndDispose(ZLinkManagedMeshNode node) =>
        _ = DrainRecords(node);

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        while (!condition())
        {
            if (Stopwatch.GetTimestamp() >= deadline)
                throw new TimeoutException("The stateful runtime condition was not reached.");
            await Task.Delay(10);
        }
    }

    private static int FindFreeTcpPort()
    {
        using var listener = new System.Net.Sockets.TcpListener(
            System.Net.IPAddress.Loopback,
            0);
        listener.Start();
        return ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
    }

    private sealed class RecordingUserSpotOperationTarget : IUserSpotOperationTarget
    {
        private int _createCount;
        private int _closeCount;

        internal int CreateCount => Volatile.Read(ref _createCount);
        internal int CloseCount => Volatile.Read(ref _closeCount);
        internal UserSpotCreateOperation LastCreate { get; private set; }
        internal UserSpotCloseOperation LastClose { get; private set; }
        internal Exception? CloseError { get; set; }

        public ValueTask<UserSpotOperationTerminal> CreateAsync(
            UserSpotCreateOperation operation,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LastCreate = operation;
            Interlocked.Increment(ref _createCount);
            return ValueTask.FromResult(new UserSpotOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new UserSpotCreateCompletion(
                    UserSpotCreateResult.Created,
                    operation.SpotId,
                    operation.Reservation.ObjectGeneration),
                [new byte[] { 0x51 }]));
        }

        public ValueTask<UserSpotOperationTerminal> CloseAsync(
            UserSpotCloseOperation operation,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LastClose = operation;
            Interlocked.Increment(ref _closeCount);
            if (CloseError is { } error)
                throw error;
            return ValueTask.FromResult(new UserSpotOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new UserSpotCloseCompletion(true)));
        }
    }

    private sealed class RecordingInstanceSpotActivationTarget
        : IInstanceSpotActivationTarget
    {
        private int _count;

        internal int Count => Volatile.Read(ref _count);
        internal InstanceSpotActivationOperation LastOperation { get; private set; }
        internal ReadOnlyMemory<byte> LastMetadata { get; private set; }
        internal IReadOnlyList<ReadOnlyMemory<byte>> LastPayload { get; private set; } =
            Array.Empty<ReadOnlyMemory<byte>>();

        public ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
            InstanceSpotActivationOperation operation,
            ReadOnlyMemory<byte>? metadata,
            IReadOnlyList<ReadOnlyMemory<byte>> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LastOperation = operation;
            LastMetadata = metadata ?? ReadOnlyMemory<byte>.Empty;
            LastPayload = payload;
            Interlocked.Increment(ref _count);
            return ValueTask.FromResult(new InstanceSpotActivationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                [new byte[] { 7, 6 }]));
        }
    }

    private sealed class ForwardingInstanceSpotActivationTarget(
        ZLinkManagedMeshNode relay,
        ZLinkManagedMeshNode winner) : IInstanceSpotActivationTarget
    {
        public async ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
            InstanceSpotActivationOperation operation,
            ReadOnlyMemory<byte>? metadata,
            IReadOnlyList<ReadOnlyMemory<byte>> payload,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var forwarded = operation with
            {
                Target = operation.Target with
                {
                    TargetNodeRid = winner.RoutingId,
                    TargetNodeGeneration = winner.Status().LifecycleGeneration,
                    DescriptorVersion = "descriptor-winner"
                }
            };
            return await relay.ForwardInstanceSpotActivationAsync(
                    forwarded,
                    payload,
                    metadata,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private sealed class BlockingInstanceSpotActivationTarget
        : IInstanceSpotActivationTarget
    {
        private readonly TaskCompletionSource _started = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource<InstanceSpotActivationTerminal>
            _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _count;

        internal int Count => Volatile.Read(ref _count);
        internal Task Started => _started.Task;

        internal void Complete() => _completion.TrySetResult(
            new InstanceSpotActivationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                [new byte[] { 227 }]));

        public async ValueTask<InstanceSpotActivationTerminal> ActivateAsync(
            InstanceSpotActivationOperation operation,
            ReadOnlyMemory<byte>? metadata,
            IReadOnlyList<ReadOnlyMemory<byte>> payload,
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref _count);
            _started.TrySetResult();
            return await _completion.Task.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private sealed class CapturingBackendActorMessageFollowHandler(
        bool acceptsOwnership,
        bool reply = true)
    {
        public int Count { get; private set; }

        public ZLinkBackendActorRouteContext LastRoute { get; private set; }

        public ZLinkServiceWireCodec.RequestSourceFence? LastRequestSource
        {
            get;
            private set;
        }

        public byte[] LastPayload { get; private set; } = [];

        public byte[] LastApplicationMetadata { get; private set; } = [];

        public IReadOnlyList<Message> LastMessages { get; private set; } = [];

        public bool DisposedByHandler { get; private set; }

        public bool TryFollow(IReadOnlyList<ZLinkBackendActorPart> parts)
        {
            Count++;
            var header = Assert.IsType<ZLinkBackendActorPart>(
                Assert.Single(parts.Take(1)));
            LastRoute = header.RouteContext;
            LastRequestSource = header.RequestSource;
            LastApplicationMetadata = header.ApplicationMetadata.ToArray();
            LastMessages = parts.Select(static part => part.Message).ToArray();
            LastPayload = parts
                .SelectMany(static part =>
                    part.Message.AsReadOnlySpan().ToArray())
                .ToArray();
            if (!acceptsOwnership)
                return false;
            try
            {
                if (reply)
                {
                    using var first = Message.From(new byte[] { 197 });
                    Assert.Equal(
                        SubmitResult.Ok,
                        header.DirectReply!([first], SendFlags.DontWait));
                    using var duplicate = Message.From(new byte[] { 199 });
                    Assert.Equal(
                        SubmitResult.Ok,
                        header.DirectReply!([duplicate], SendFlags.DontWait));
                }
                return true;
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(LastMessages);
                DisposedByHandler = true;
            }
        }
    }

    private sealed class ProductionUserSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        private static int _createCount;
        private static int _closeCount;
        private static int _cleanupTokenWasCanceledAtInvocation;
        private static string? _lastCreateContentType;
        private static ZLinkSpotClosingContext? _lastClosingContext;
        private static ulong _objectGenerationObservedDuringCreate;
        private static CreateGate? _nextCreateGate;

        public IZLinkSpotContext Context { get; } = context;
        internal static int CreateCount => Volatile.Read(ref _createCount);
        internal static int CloseCount => Volatile.Read(ref _closeCount);
        internal static bool CleanupTokenWasCanceledAtInvocation =>
            Volatile.Read(ref _cleanupTokenWasCanceledAtInvocation) != 0;
        internal static string? LastCreateContentType =>
            Volatile.Read(ref _lastCreateContentType);
        internal static ZLinkSpotClosingContext? LastClosingContext =>
            _lastClosingContext;
        internal static ulong ObjectGenerationObservedDuringCreate =>
            _objectGenerationObservedDuringCreate;

        internal static void Reset()
        {
            Volatile.Write(ref _createCount, 0);
            Volatile.Write(ref _closeCount, 0);
            Volatile.Write(ref _cleanupTokenWasCanceledAtInvocation, 0);
            Volatile.Write(ref _lastCreateContentType, null);
            _lastClosingContext = null;
            _objectGenerationObservedDuringCreate = 0;
            Volatile.Write(ref _nextCreateGate, null);
        }

        internal static CreateGate BlockNextCreate()
        {
            var gate = new CreateGate();
            if (Interlocked.CompareExchange(ref _nextCreateGate, gate, null)
                is not null)
                throw new InvalidOperationException("A create gate is already installed.");
            return gate;
        }

        public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref _createCount);
            Volatile.Write(ref _lastCreateContentType, request.ContentType);
            _objectGenerationObservedDuringCreate = Context.ObjectGeneration;
            if (Interlocked.Exchange(ref _nextCreateGate, null) is { } gate)
            {
                gate.Entered.TrySetResult();
                await gate.Release.Task.WaitAsync(cancellationToken);
            }
            return ZLinkSpotCreateResponse.Accept(
                new ProductionCreateReply("production-created"));
        }

        public ValueTask OnClosingAsync(
            ZLinkSpotClosingContext context,
            CancellationToken cleanupCancellationToken)
        {
            _lastClosingContext = context;
            Volatile.Write(
                ref _cleanupTokenWasCanceledAtInvocation,
                cleanupCancellationToken.IsCancellationRequested ? 1 : 0);
            cleanupCancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref _closeCount);
            return ValueTask.CompletedTask;
        }

        internal sealed class CreateGate
        {
            internal TaskCompletionSource Entered { get; } =
                new(TaskCreationOptions.RunContinuationsAsynchronously);
            internal TaskCompletionSource Release { get; } =
                new(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    private sealed class MonitoringInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;
    }

    private sealed record ProductionCreateReply(string Value);

    private sealed class TestRelocationStore : IZLinkRelocationRepository
    {
        private readonly Dictionary<string, byte[]> _payloads =
            new(StringComparer.Ordinal);

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var bytes = payload.ToArray();
            var reference = Convert.ToHexString(
                System.Security.Cryptography.SHA256.HashData(bytes));
            _payloads[reference] = bytes;
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            if (_payloads.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("Relocation reference collision.");
            _payloads[reference] = bytes;
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                Zlink.Framework.Runtime.Locations.ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkRelocationReadResult>(
                _payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkRelocationRenewResult>(
                _payloads.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(now + retention, now)
                    : new ZLinkRelocationRenewResult.Missing());
        }

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                _payloads.Remove(reference)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
    }
}
