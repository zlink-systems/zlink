using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    private static JsonDocument LoadAuthorityFollowFixture()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(
                directory.FullName,
                "framework/runtime/conformance/spot-close-v1.json"
            );
            if (File.Exists(path))
                return JsonDocument.Parse(File.ReadAllText(path));
            directory = directory.Parent;
        }
        throw new FileNotFoundException("spot-close-v1.json");
    }

    public static IEnumerable<object[]> AuthorityFollowCases()
    {
        using var fixture = LoadAuthorityFollowFixture();
        foreach (
            var scenario in fixture.RootElement.GetProperty("authorityFollowCases").EnumerateArray()
        )
            yield return [scenario.GetProperty("name").GetString()!];
    }

    [Theory]
    [MemberData(nameof(AuthorityFollowCases))]
    public async Task Instance_intent_follows_authority_with_original_deadline(string name)
    {
        using var fixture = LoadAuthorityFollowFixture();
        var scenario = fixture
            .RootElement.GetProperty("authorityFollowCases")
            .EnumerateArray()
            .Single(row => row.GetProperty("name").GetString() == name);
        var refusals = scenario.GetProperty("refusals").GetInt32();
        var intent = scenario.GetProperty("intent").GetBoolean();
        var missing = scenario.GetProperty("authority").GetString() == "Missing";
        var creating = scenario.GetProperty("authority").GetString() == "Creating";
        HeldCreatingCommitStore? held = null;
        Task<IReadOnlyList<Message>>? creation = null;
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(RecoveryColdSpot),
            relocationStore: new InMemoryRelocationStore(),
            locationStoreWrapper: creating
                ? inner => held = new HeldCreatingCommitStore(inner, "spot-ready")
                : null
        );
        try
        {
            var createdAtStart = node.CreatedSpots.Count;
            ulong? creatingGeneration = null;
            var store = RequireLocationStore(runtime);
            var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot-ready");
            var attempts = 0;
            ZLinkRequestAdmission? originalAdmission = null;
            node.SpotRequestSubmissionHandler = async (admission, parts) =>
            {
                originalAdmission ??= admission;
                Assert.Same(originalAdmission, admission);
                admission?.Complete();
                if (++attempts == 1)
                {
                    var found = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                        await store.ReadAuthorityAsync(key)
                    );
                    if (missing || creating)
                        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
                            await store.CompareExchangeAuthorityAsync(
                                key,
                                found.Snapshot.StoreVersion,
                                new ZLinkAuthorityMutation.Delete()
                            )
                        );
                    else
                    {
                        Assert.True(
                            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                                found.Snapshot.Payload.Span,
                                out var payload
                            )
                        );
                        var updated = ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
                            payload! with
                            {
                                NodeRid = RoutingId.From("new-owner"),
                            }
                        );
                        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                            await store.CompareExchangeAuthorityAsync(
                                key,
                                found.Snapshot.StoreVersion,
                                new ZLinkAuthorityMutation.Put(
                                    updated,
                                    ZLinkAuthorityGenerationTransition.Preserve,
                                    null,
                                    null
                                )
                            )
                        );
                    }
                }
                if (creating && attempts == 1)
                {
                    held!.Enabled = true;
                    creation = runtime
                        .ActivateInstanceSpotAsync(
                            new InstanceSpotIntentAddress(
                                "entry",
                                "Tests.InstanceSpot",
                                "spot-ready"
                            ),
                            parts,
                            request: true,
                            TimeSpan.FromSeconds(2),
                            default,
                            CancellationToken.None
                        )
                        .AsTask();
                    await held.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
                }
                if (refusals < 0 || attempts <= refusals)
                    throw new ZLinkFrameworkException(
                        scenario.TryGetProperty("failureCode", out var code)
                        && code.GetInt32() == 33
                            ? ZLinkFrameworkErrorKind.InvalidOperation
                            : ZLinkFrameworkErrorKind.Unavailable,
                        "pre-admission owner fence"
                    )
                    {
                        Origin = ZLinkErrorOrigin.Framework,
                        FrameworkFailureCode = scenario.TryGetProperty(
                            "failureCode",
                            out var failureCode
                        )
                            ? failureCode.GetInt32()
                            : 34,
                    };
                Assert.Equal(RoutingId.From("new-owner"), node.SpotRequests.Last().NodeRid);
                return new ZLinkBackendRouteReceived(
                    ZLinkEnvelopeCodec.EncodeParts(
                        ZLinkEnvelopeCodec.DecodeHeader(parts) with
                        {
                            Kind = ZLinkMessageKind.Response,
                            MessageName = string.Empty,
                        },
                        new ProbeReply("authority-reply"),
                        typeof(ProbeReply),
                        codecs: null
                    ),
                    null,
                    null,
                    null,
                    null
                );
            };
            var call = intent
                ? new ZLinkInstanceSpotRequestCall<ProbeRouteMessage>(
                    runtime,
                    new InstanceSpotIntentAddress("entry", "Tests.InstanceSpot", "spot-ready"),
                    new ProbeRouteMessage("authority")
                )
                : new ZLinkInstanceSpotRequestCall<ProbeRouteMessage>(
                    runtime,
                    "spot-ready",
                    new ProbeRouteMessage("authority")
                );
            call.Timeout(refusals < 0 ? TimeSpan.FromMilliseconds(40) : TimeSpan.FromSeconds(2));
            if (scenario.GetProperty("terminal").GetString() == "reply")
            {
                var pending = call.Async<ProbeReply>().AsTask();
                if (creating)
                {
                    await held!.ObservedCreating.Task.WaitAsync(TimeSpan.FromSeconds(5));
                    Assert.False(pending.IsCompleted);
                    var reserved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                        await store.ReadAuthorityAsync(key)
                    );
                    Assert.Equal(
                        ZLinkPlacementAllocationState.Reserved,
                        reserved.Snapshot.Allocation.State
                    );
                    Assert.NotNull(reserved.Snapshot.ReservedCreation);
                    creatingGeneration = reserved.Snapshot.ObjectGeneration;
                    held.Release.TrySetResult();
                    ZLinkMessageParts.DisposeAll(await creation!);
                }
                var reply = await pending;
                Assert.Equal("authority-reply", reply.Value);
                if (creating)
                {
                    var ready = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                        await store.ReadAuthorityAsync(key)
                    );
                    Assert.Equal(creatingGeneration, ready.Snapshot.ObjectGeneration);
                    Assert.Equal(createdAtStart + 1, node.CreatedSpots.Count);
                }
                Assert.Equal(refusals + (missing || creating ? 0 : 1), attempts);
            }
            else
            {
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    call.Async<ProbeReply>().AsTask()
                );
                Assert.Equal(
                    refusals < 0
                        ? ZLinkFrameworkErrorKind.DeadlineExceeded
                        : ZLinkFrameworkErrorKind.Unavailable,
                    failure.Kind
                );
                if (refusals >= 0)
                    Assert.Equal(1, attempts);
            }
        }
        finally
        {
            held?.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class HeldCreatingCommitStore(IZLinkLocationStore inner, string spotId)
        : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey key = ZLinkProviderLocationRepository.AuthorityMetaKey(
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
        );
        private int writes;
        public bool Enabled { get; set; }
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ObservedCreating { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey candidate,
            CancellationToken cancellationToken = default
        )
        {
            var result = await inner.ReadAsync(candidate, cancellationToken);
            if (candidate == key && Entered.Task.IsCompleted && !Release.Task.IsCompleted)
                ObservedCreating.TrySetResult();
            return result;
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                Enabled
                && request.Mutations.Any(mutation =>
                    mutation is ZLinkStoreMutation.Put put && put.Key == key
                )
                && Interlocked.Increment(ref writes) == 2
            )
            {
                Entered.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
            }
            return await inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
