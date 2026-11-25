using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Systems.Zlink;
using Xunit;
using Xunit.Abstractions;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Tests;

internal static class LocationCreationContentionContract
{
    public static async Task VerifyAsync(
        IZLinkLocationStore provider,
        string caseId,
        ITestOutputHelper output
    )
    {
        var scenario = LocationContentionFixture.ReadScenario("distinctActorCreations");
        var settings = scenario.GetProperty("cases").GetProperty(caseId);
        var expected = scenario.GetProperty("expected");
        var actorCount = scenario.GetProperty("actorCount").GetInt32();
        var actorLimit = settings.GetProperty("actorLimit").GetInt32();
        var actorIdPrefix = scenario.GetProperty("actorIdPrefix").GetString()!;
        var actorType = scenario.GetProperty("actorType").GetString()!;
        var mesh = scenario.GetProperty("mesh").GetString()!;
        var measured = new MeasuredStore(provider, actorCount);
        var repository = new ZLinkProviderLocationRepository(measured);
        await repository.EnsureStoreFormatAsync(CancellationToken.None);
        var owner = Assert
            .IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
                await repository.ClaimOwnerLeaseAsync("creation-owner", TimeSpan.FromMinutes(5))
            )
            .Token;
        var rid = RoutingId.From(mesh);
        var descriptor = new ZLinkMeshNodeDescriptor(
            mesh,
            rid,
            1,
            1,
            "tcp://127.0.0.1:7101",
            new Dictionary<string, int> { [mesh] = 100 },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default
        )
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    actorType,
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0
                ),
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, actorLimit),
                new ZLinkPopulationCapacity(0, 0, 0),
                []
            ),
            EntrySpotId = $"{mesh}-entry-00000000-0000-4000-8000-000000000001",
            State = ZLinkFrameworkRuntimeState.Serving,
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (
                await repository.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)
            ).Status
        );

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        var cancellationToken = timeout.Token;
        var observations = await Task.WhenAll(Enumerable.Range(0, actorCount).Select(Create));
        var conflicts = observations.Sum(observation => observation.Conflicts);
        output.WriteLine(
            $"Creations={observations.Length} limit={actorLimit} conflicts={conflicts} "
                + $"writes/creation={observations.Min(o => o.Writes)}..{observations.Max(o => o.Writes)} "
                + $"reads/creation={observations.Min(o => o.Reads)}..{observations.Max(o => o.Reads)} "
                + $"scans/creation={observations.Min(o => o.Scans)}..{observations.Max(o => o.Scans)} "
                + $"total-writes={observations.Sum(o => o.Writes)} "
                + $"total-reads={observations.Sum(o => o.Reads)} "
                + $"total-scans={observations.Sum(o => o.Scans)}"
        );

        Assert.Equal(expected.GetProperty("createdActors").GetInt32(), observations.Length);
        Assert.Equal(expected.GetProperty("writeConflicts").GetInt32(), conflicts);
        Assert.Equal(
            settings.GetProperty("expectedReservationConditions").GetInt32(),
            observations.Sum(observation => observation.ReservationLimits)
        );
        var writesPerActor = expected.GetProperty("authorityWritesPerActor").GetInt32();
        var objectGeneration = expected.GetProperty("objectGeneration").GetUInt64();
        Assert.All(
            observations,
            observation =>
            {
                Assert.Equal(writesPerActor, observation.Writes);
                Assert.Equal(actorLimit == 0 ? 0 : 1, observation.ReservationLimits);
                Assert.Equal(0, observation.CompletionLimits);
                Assert.Equal(objectGeneration, observation.ObjectGeneration);
                Assert.Equal(1UL, observation.AuthorityOwnerGeneration);
            }
        );
        if (actorLimit > 0)
            Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(
                await repository.ReserveAsync(Reservation(actorCount), cancellationToken)
            );

        var capacityKey = $"zlink:v12:capacity\0{descriptor.MeshName}\0{rid.ToHex()}\0actors";
        var active = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(new ZLinkStoreKey($"{capacityKey}\0active"), cancellationToken)
        );
        var pending = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(
                new ZLinkStoreKey($"{capacityKey}\0pending"),
                cancellationToken
            )
        );
        Assert.Equal(
            expected
                .GetProperty("activeActorSlots")
                .GetInt64()
                .ToString(CultureInfo.InvariantCulture),
            Encoding.UTF8.GetString(active.Value.Bytes.Span)
        );
        Assert.Equal(
            expected
                .GetProperty("pendingActorSlots")
                .GetInt64()
                .ToString(CultureInfo.InvariantCulture),
            Encoding.UTF8.GetString(pending.Value.Bytes.Span)
        );

        async Task<CreationObservation> Create(int index)
        {
            var observation = new CreationObservation();
            measured.Current.Value = observation;
            try
            {
                var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                    await repository.ReserveAsync(Reservation(index), cancellationToken)
                );
                observation.ObjectGeneration = reserved.Reservation.ObjectGeneration;
                observation.AuthorityOwnerGeneration = reserved
                    .Reservation
                    .AuthorityOwnerGeneration;
                observation.Reserved = true;
                Assert.IsType<ZLinkObjectCreationCompleteResult.Created>(
                    await repository.CompleteCreationAsync(
                        reserved.Reservation,
                        new ZLinkObjectCreationCompletion.Created(
                            new byte[] { 0x22 },
                            new ZLinkCreationTerminalPublication(
                                new ZLinkCreationOperationId(
                                    rid,
                                    descriptor.LifecycleGeneration,
                                    0,
                                    (ulong)index + 1
                                ),
                                new byte[] { 0x54 },
                                DateTimeOffset.UtcNow.AddMinutes(5)
                            )
                        ),
                        cancellationToken
                    )
                );
                return observation;
            }
            finally
            {
                measured.Current.Value = null;
            }
        }

        ZLinkObjectReservationRequest Reservation(int index)
        {
            var actorId = $"{actorIdPrefix}{index.ToString(CultureInfo.InvariantCulture)}";
            var intent = Encoding.UTF8.GetBytes($"create:{actorId}");
            return new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.Actor,
                ZLinkAuthorityKeyCodec.EncodeActor(actorId),
                actorType,
                $"inline:{actorId}",
                SHA256.HashData(intent),
                intent.Length,
                new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, rid),
                descriptor.LifecycleGeneration,
                owner,
                new byte[] { 0x11 },
                new ZLinkCapacityVector(1, 0, null)
            );
        }
    }

    private sealed class CreationObservation
    {
        public int Reads;
        public int Scans;
        public int Writes { get; set; }
        public int Conflicts { get; set; }
        public int ReservationLimits { get; set; }
        public int CompletionLimits { get; set; }
        public ulong ObjectGeneration { get; set; }
        public ulong AuthorityOwnerGeneration { get; set; }
        public bool Reserved { get; set; }
    }

    private sealed class MeasuredStore(IZLinkLocationStore inner, int actorCount)
        : IZLinkLocationStore
    {
        private readonly TaskCompletionSource _firstWrites = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _arrived;

        public AsyncLocal<CreationObservation?> Current { get; } = new();

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (Current.Value is { } observation)
                Interlocked.Increment(ref observation.Reads);
            return inner.ReadAsync(key, cancellationToken);
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            var observation = Current.Value;
            if (observation is null)
                return await inner.WriteAsync(request, cancellationToken);

            var limits = request.Conditions.Count(condition =>
                condition is ZLinkStoreCondition.SumAtMost
            );
            if (observation.Reserved)
                observation.CompletionLimits += limits;
            else
                observation.ReservationLimits += limits;

            // Collect every first reservation after its reads and before any write reaches the store.
            if (++observation.Writes == 1)
            {
                if (Interlocked.Increment(ref _arrived) == actorCount)
                    _firstWrites.SetResult();
                await _firstWrites.Task.WaitAsync(cancellationToken);
            }
            var result = await inner.WriteAsync(request, cancellationToken);
            if (result is ZLinkStoreWriteResult.Conflict)
                observation.Conflicts++;
            return result;
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (Current.Value is { } observation)
                Interlocked.Increment(ref observation.Scans);
            return inner.ScanAsync(request, cancellationToken);
        }
    }
}
