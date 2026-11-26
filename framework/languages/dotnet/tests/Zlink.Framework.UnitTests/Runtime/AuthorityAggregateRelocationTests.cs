using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class AuthorityAggregateRelocationTests
{
    [Fact]
    public void Shared_aggregate_records_round_trip_and_invalid_records_fail_closed()
    {
        using var golden = ReadGolden();
        foreach (var item in golden.RootElement.GetProperty("cases").EnumerateArray())
        {
            var aggregate = item.GetProperty("aggregate");
            if (item.GetProperty("expect").GetString() == "reject")
            {
                Assert.Throws<InvalidDataException>(() =>
                    ZLinkAuthorityAggregateCodec.Read(aggregate)
                );
                continue;
            }
            var relocation = ZLinkAuthorityAggregateCodec.Read(aggregate);
            var rewritten = ZLinkAuthorityAggregateCodec.Replace(aggregate, relocation);
            Assert.NotNull(rewritten);
            Assert.Equal(
                JsonSerializer.Serialize(aggregate),
                JsonSerializer.Serialize(rewritten.Value)
            );
        }
    }

    [Fact]
    public void Single_actor_inventory_hashes_the_canonical_utf8_of_opaque_store_versions()
    {
        var digest = ZLinkDirectRelocationInventory.Digest(
            ZLinkAuthorityKeyCodec.EncodeActor("golden-actor"),
            "v\"\u2028\U0001f680\n\u0001",
            "abc"u8.ToArray()
        );
        Assert.Equal("c0a435f32ba87bb4687a6a721cac6023f65e30bb9a2f6441b649b9fdd15d40ae", digest);
    }

    private static JsonDocument ReadGolden()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (!Directory.Exists(Path.Combine(directory.FullName, "framework", "runtime")))
            directory = directory.Parent!;
        return JsonDocument.Parse(
            File.ReadAllBytes(
                Path.Combine(
                    directory.FullName,
                    "framework/runtime/protocol/golden/authority-aggregate-relocation-v1.json"
                )
            )
        );
    }

    [Fact]
    public async Task Capture_writes_the_shared_record_and_foreign_capture_holds_the_fence()
    {
        using var golden = ReadGolden();
        var expected = golden
            .RootElement.GetProperty("cases")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == "relocation-only-captured")
            .GetProperty("aggregate");
        var capturedRecord = expected.GetProperty("relocation");
        var relocationId = capturedRecord.GetProperty("relocationId").GetGuid();
        var provider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(provider);
        var owner = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await store.ClaimOwnerLeaseAsync("owner-src", TimeSpan.FromMinutes(1))
            )
            .Token;
        var targetOwner = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await store.ClaimOwnerLeaseAsync("owner-tgt", TimeSpan.FromMinutes(1))
            )
            .Token;
        var source = StandaloneActorRelocationPrecommitTests.Descriptor(
            RoutingId.FromHex("0a0b"),
            owner
        ) with
        {
            LifecycleGeneration = 3,
        };
        var target = StandaloneActorRelocationPrecommitTests.Descriptor(
            RoutingId.FromHex("0c0d"),
            targetOwner
        ) with
        {
            LifecycleGeneration = 5,
        };
        await store.UpdateMeshNodeAsync(source, ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(target, ZLinkLocationWriteIntent.NewClaim);
        var actor = StandaloneActorRelocationPrecommitTests.Authority(
            "golden-actor",
            source,
            owner
        );
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.Actor,
                    key,
                    "Game.Actor",
                    "golden-intent",
                    SHA256.HashData("intent"u8),
                    6,
                    new("mesh", source.Rid),
                    source.LifecycleGeneration,
                    owner,
                    new byte[] { 1 },
                    new(1, 0, null)
                )
            )
        );
        var steady = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await store.CommitAsync(
                    reserved.Reservation,
                    ZLinkActorAuthorityPayloadCodec.Encode(actor)
                )
            )
            .Snapshot;
        var coordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(store);
        var preparing = await coordinator.BeginPreparingAsync(
            steady,
            actor,
            relocationId,
            42,
            CancellationToken.None
        );
        var root = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                preparing,
                actor,
                target,
                relocationId,
                ReadOnlyMemory<byte>.Empty,
                [],
                default
            ),
            42
        );
        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            steady,
            actor,
            target,
            root,
            ZLinkRelocationTransferPayload.Create(root, 1024),
            42,
            DateTimeOffset.UtcNow.AddMinutes(1)
        );
        var captured = await coordinator.CaptureAsync(preparing, root, CancellationToken.None);
        var rowKey = ZLinkProviderLocationRepository.AuthorityMetaKey(key);
        var row = Assert.IsType<ZLinkStoreReadResult.Found>(await provider.ReadAsync(rowKey));
        using var document = JsonDocument.Parse(row.Value.Bytes);
        var actual = document.RootElement.GetProperty("aggregate").GetProperty("relocation");
        Assert.Equal("captured", actual.GetProperty("phase").GetString());
        Assert.Equal("0a0b", actual.GetProperty("source").GetProperty("nodeRid").GetString());
        Assert.Equal(steady.Payload.ToArray(), captured.Payload.ToArray());
        Assert.Equal(
            preparing.Relocation!.SourceInventorySha256,
            actual.GetProperty("sourceInventorySha256").GetString()
        );
        Assert.NotEqual(
            preparing.StoreVersion,
            actual.GetProperty("coordinator").GetProperty("expectedStoreVersion").GetString()
        );
        Assert.Equal(
            steady.StoreVersion,
            actual.GetProperty("coordinator").GetProperty("expectedStoreVersion").GetString()
        );

        var fence = new ZLinkAggregateFence(relocationId, root.AggregateGeneration);
        var targetRepository = new ZLinkProviderLocationRepository(provider);
        var targetCoordinator = new ZLinkStandaloneActorRelocationPrecommitCoordinator(
            targetRepository
        );
        await targetCoordinator.VerifyCapturedInventoryAsync(
            captured,
            root,
            prepare,
            CancellationToken.None
        );

        var mutationKey = ZLinkAggregateInventory.ParticipantKey(
            "zlink:v11:",
            fence,
            0,
            membership: true
        );
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new ZLinkStoreWriteRequest([], [new ZLinkStoreMutation.Delete(mutationKey)])
            )
        );
        var registration = new ZLinkFrameworkRegistration();
        registration.Locations._repository = targetRepository;
        var targetRuntime = new ZLinkStandaloneActorRelocationRuntime(null!, null!, registration);
        var failure = await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            targetRuntime
                .StageTargetAsync(prepare, root, prepare.SourceNodeRid, CancellationToken.None)
                .AsTask()
        );
        Assert.Contains(mutationKey.Value, failure.Message, StringComparison.Ordinal);
        Assert.Empty(targetRuntime.SnapshotPendingAttemptNames());

        var state = captured.Relocation!.Project(captured.Payload, root.AggregateGeneration).State;
        var legacy = captured with
        {
            Relocation = null,
            Payload = ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                steady.Payload.Span,
                state,
                root
            ),
        };
        Assert.True(
            ZLinkStandaloneActorRelocationPrecommitCoordinator.HoldsTargetCommitFence(
                legacy,
                root,
                prepare
            )
        );
        Assert.False(
            ZLinkStandaloneActorRelocationPrecommitCoordinator.HoldsTargetCommitFence(
                legacy with
                {
                    Relocation = preparing.Relocation,
                },
                root,
                prepare
            )
        );

        var foreign = JsonNode.Parse(row.Value.Bytes.Span)!.AsObject();
        foreign["aggregate"] = JsonNode.Parse(expected.GetRawText());
        foreign["ownerLeaseGeneration"] = "7";
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [
                        new ZLinkStoreMutation.Put(
                            rowKey,
                            JsonSerializer.SerializeToUtf8Bytes(foreign),
                            null
                        ),
                    ]
                )
            )
        );
        var snapshot = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key, CancellationToken.None)
            )
            .Snapshot;
        var foreignPrepare = prepare with
        {
            Coordinator = prepare.Coordinator with
            {
                LeaseGeneration = 7,
                ExpectedAuthorityStoreVersion = "opaque-v1",
            },
        };
        Assert.True(
            ZLinkStandaloneActorRelocationPrecommitCoordinator.HoldsTargetCommitFence(
                snapshot,
                root,
                foreignPrepare
            )
        );
        Assert.False(
            ZLinkStandaloneActorRelocationPrecommitCoordinator.HoldsTargetCommitFence(
                snapshot,
                root,
                foreignPrepare with
                {
                    Coordinator = foreignPrepare.Coordinator with
                    {
                        ExpectedAuthorityStoreVersion = snapshot.StoreVersion,
                    },
                }
            )
        );

        var prepared = golden
            .RootElement.GetProperty("cases")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == "relocation-only-prepared")
            .GetProperty("aggregate");
        Assert.True(
            ZLinkStandaloneActorRelocationPrecommitCoordinator.IsSourcePrecommit(
                snapshot with
                {
                    Relocation = ZLinkAuthorityAggregateCodec.Read(prepared),
                },
                relocationId
            )
        );

        var committed = golden
            .RootElement.GetProperty("cases")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == "marker-and-committed")
            .GetProperty("aggregate");
        foreign["aggregate"] = JsonNode.Parse(committed.GetRawText());
        foreign["aggregate"]!["relocation"]!["target"]!["ownerLeaseGeneration"] =
            targetOwner.LeaseGeneration.ToString(System.Globalization.CultureInfo.InvariantCulture);
        foreign["ownerId"] = targetOwner.OwnerId;
        foreign["ownerLeaseGeneration"] = targetOwner.LeaseGeneration.ToString(
            System.Globalization.CultureInfo.InvariantCulture
        );
        foreign["authorityOwnerGeneration"] = "12";
        foreign["allocation"]!["descriptor"]!["routingIdHex"] = "0c0d";
        foreign["allocation"]!["descriptorLifecycleGeneration"] = "5";
        // A committed marker needs its aggregate row. For this direct Join
        // authority use the same relocation record without the marker fields.
        foreign["aggregate"] = new JsonObject
        {
            ["relocation"] = foreign["aggregate"]!["relocation"]!.DeepClone(),
        };
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [
                        new ZLinkStoreMutation.Put(
                            rowKey,
                            JsonSerializer.SerializeToUtf8Bytes(foreign),
                            null
                        ),
                    ]
                )
            )
        );
        var targetAuthority = StandaloneActorRelocationPrecommitTests.Authority(
            actor.ActorId,
            target,
            targetOwner
        );
        var exactTarget = foreignPrepare with { TargetAttemptGeneration = 2 };
        var exactPreparedSource = snapshot with
        {
            Relocation = snapshot.Relocation! with
            {
                Phase = Systems
                    .Zlink
                    .Framework
                    .Runtime
                    .Protocol
                    .ServiceWireCodec
                    .RelocationPhase
                    .Prepared,
                Target = new(
                    exactTarget.Target.NodeRid.ToHex(),
                    exactTarget.Target.NodeGeneration,
                    exactTarget.Target.OwnerId,
                    exactTarget.Target.OwnerLeaseGeneration,
                    exactTarget.TargetAttemptGeneration
                ),
            },
        };
        var observed = await coordinator.CommitTargetAsync(
            exactPreparedSource,
            root,
            exactTarget,
            targetAuthority,
            () => true,
            TimeSpan.Zero,
            CancellationToken.None
        );
        Assert.Equal(targetOwner.OwnerId, observed.OwnerId);
        Assert.Equal((ulong)2, observed.Relocation!.Target!.AttemptGeneration);
        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.CommitTargetAsync(
                exactPreparedSource,
                root,
                exactTarget with
                {
                    TargetAttemptGeneration = 3,
                },
                targetAuthority,
                () => true,
                TimeSpan.Zero,
                CancellationToken.None
            )
        );

        foreign["aggregate"]!["relocation"]!["coordinator"]!["expectedStoreVersion"] =
            snapshot.StoreVersion;
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [
                        new ZLinkStoreMutation.Put(
                            rowKey,
                            JsonSerializer.SerializeToUtf8Bytes(foreign),
                            null
                        ),
                    ]
                )
            )
        );
        await Assert.ThrowsAsync<ZLinkRelocationTargetSettledException>(async () =>
            await coordinator.CommitTargetAsync(
                exactPreparedSource,
                root,
                exactTarget,
                targetAuthority,
                () => true,
                TimeSpan.Zero,
                CancellationToken.None
            )
        );
    }
}
