using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

//  Actor model §6.3: a new Actor is created only on an Object Server whose
//  registered Entry Spot type accepts that Actor stable type
//  (descriptor capability `entryAccepted`, Location runtime §5).
public sealed partial class EntrySpotActorDispatchTests
{
    private static readonly RoutingId AcceptingNodeRid = RoutingId.From("accepting-node");

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ActorCapability_PublishesEntryAcceptedFromRegisteredEntrySpotType(
        bool entryAcceptsActor
    )
    {
        //  The MeshNode descriptor factory is the one publication path; the
        //  stored Location Store row carries the canonical `entryAccepted`.
        var provider = new ZLinkInMemoryProviderLocationStore(new ManualTimeProvider());
        var store = new ZLinkProviderLocationRepository(provider);
        var owner = await store.ClaimLiveOwnerAsync("capability-owner", TimeSpan.FromMinutes(1));
        var descriptor = CreateNodeDescriptor(
            new ZLinkFrameworkRegistration(),
            AcceptingNodeRid,
            entryAcceptsActor ? typeof(AcceptingProbeEntrySpot) : typeof(ProbeEntrySpot)
        ) with
        {
            OwnerId = owner.OwnerId,
            LeaseGeneration = owner.LeaseGeneration,
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)).Status
        );

        var read = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(
                ZLinkProviderLocationRepository.MeshKey("entry", AcceptingNodeRid)
            )
        );
        using var parsed = JsonDocument.Parse(read.Value.Bytes);
        var capability = Assert.Single(
            parsed
                .RootElement.GetProperty("descriptor")
                .GetProperty("objectCapabilities")
                .EnumerateArray()
        );

        Assert.Equal("actor", capability.GetProperty("objectKind").GetString());
        Assert.Equal("probe", capability.GetProperty("stableType").GetString());
        Assert.True(
            capability.TryGetProperty("entryAccepted", out var entryAccepted),
            capability.GetRawText()
        );
        Assert.Equal(entryAcceptsActor, entryAccepted.GetBoolean());
    }

    [Fact]
    public async Task ActorCreate_SelectsOnlyNodeWhoseEntrySpotAcceptsActorType()
    {
        //  The local node registers the "probe" factory but its Entry Spot
        //  accepts no Actor type; only the remote node's Entry accepts "probe".
        var node = new CapturingSpotNode();
        AdmitAcceptingPeer(node);
        var (runtime, _) = await CreateStartedRuntimeAsync(node, preseedActorOwnership: false);
        try
        {
            var store = RequireLocationStore(runtime);
            var local = Assert.Single(
                (await store.ListMeshNodesAsync("entry", new ZLinkPageRequest(100))).Items,
                descriptor => descriptor.Rid == node.RoutingId
            );
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (
                    await store.UpdateMeshNodeAsync(
                        local with
                        {
                            DescriptorRevision = checked(local.DescriptorRevision + 1),
                            ObjectCapabilities = CreateNodeDescriptor(
                                runtime.Registration,
                                node.RoutingId,
                                typeof(ProbeEntrySpot)
                            ).ObjectCapabilities,
                        },
                        ZLinkLocationWriteIntent.Renew
                    )
                ).Status
            );
            await PublishAcceptingNodeAsync(runtime, store);
            var remoteCreates = HandleRemoteCreatesOnAcceptingNode(node, runtime, store);
            var manager = new ZLinkActorManagerService(runtime);

            for (var index = 0; index < 4; index++)
            {
                var result = await manager
                    .Create($"entry-candidate-{index}", "probe")
                    .InMesh("entry")
                    .Async();

                var created = Assert.IsType<ZLinkActorCreateResult.Created>(result);
                Assert.Equal(AcceptingNodeRid, created.Actor.NodeRid);
                Assert.False(runtime.TryGetCreatedActorState($"entry-candidate-{index}", out _));
            }
            Assert.Equal(4, remoteCreates.Count);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorCreate_StaleDescriptorTargetRefusesBeforeFactoryAndRequesterSelectsAnother()
    {
        //  The local descriptor is stale: it still claims an Entry Spot that
        //  accepts "probe", but the local registration accepts no Actor type.
        //  The local target must end the creation before the factory runs,
        //  and the requester excludes that host run generation and creates
        //  the Actor on the remaining accepting node within the deadline.
        var node = new CapturingSpotNode();
        AdmitAcceptingPeer(node);
        var (runtime, _) = await CreateStartedRuntimeAsync(node, preseedActorOwnership: false);
        try
        {
            var store = RequireLocationStore(runtime);
            var accepting = await PublishAcceptingNodeAsync(runtime, store);
            var local = Assert.Single(
                (await store.ListMeshNodesAsync("entry", new ZLinkPageRequest(100))).Items,
                descriptor => descriptor.Rid == node.RoutingId
            );
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (
                    await store.UpdateMeshNodeAsync(
                        local with
                        {
                            DescriptorRevision = checked(local.DescriptorRevision + 1),
                            ObjectCapabilities = accepting.ObjectCapabilities,
                        },
                        ZLinkLocationWriteIntent.Renew
                    )
                ).Status
            );
            var remoteCreates = HandleRemoteCreatesOnAcceptingNode(node, runtime, store);
            var manager = new ZLinkActorManagerService(runtime);

            for (var index = 0; index < 4; index++)
            {
                var actorId = $"stale-entry-candidate-{index}";
                var result = await manager.Create(actorId, "probe").InMesh("entry").Async();

                var created = Assert.IsType<ZLinkActorCreateResult.Created>(result);
                Assert.Equal(AcceptingNodeRid, created.Actor.NodeRid);
                Assert.False(runtime.TryGetCreatedActorState(actorId, out _));
                var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
                    )
                );
                Assert.Equal(AcceptingNodeRid, authority.Snapshot.Allocation.Descriptor.Rid);
            }
            Assert.Equal(4, remoteCreates.Count);
            //  The native Actor is created right before the factory runs.
            Assert.Empty(node.CreatedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void ActorEntryJoinCandidate_RequiresAnEntrySpotThatAcceptsTheActorType()
    {
        // Membership §4 applies the Entry acceptance requirement to target
        // selection for Entry Spot Join as well as new Actor placement.
        var framework = new ZLinkFrameworkRegistration();
        var descriptor = CreateNodeDescriptor(
            framework,
            AcceptingNodeRid,
            entrySpotType: typeof(ProbeEntrySpot)
        );

        Assert.False(ZLinkActorManagerService.IsEligibleCandidate(descriptor, "probe"));
    }

    [Theory]
    [InlineData("entry-not-accepted", true)]
    [InlineData("entry-not-accepted", false)]
    [InlineData("not-serving", true)]
    [InlineData("wrong-type", true)]
    [InlineData("wrong-kind", true)]
    [InlineData("client-role", true)]
    [InlineData("no-entry", true)]
    [InlineData("zero-weight", true)]
    [InlineData("active-full", true)]
    [InlineData("pending-full", true)]
    [InlineData("empty", true)]
    [InlineData("expired-owner", true)]
    public async Task EntryJoinSelection_NoEligibleTargetIsUnavailableAndPreservesMembership(
        string excludedCondition,
        bool inUserSpot
    )
    {
        var node = new CapturingSpotNode();
        var time = new ManualTimeProvider();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            locationTimeProvider: time,
            entrySpotType: typeof(AcceptingProbeEntrySpot),
            userSpotType: typeof(EntryReturnSourceSpot),
            meshResolverWrapper: resolver => new EntryJoinDescriptors(
                resolver,
                descriptors =>
                    excludedCondition == "empty"
                        ? []
                        : descriptors
                            .Select(descriptor =>
                                excludedCondition switch
                                {
                                    "expired-owner" => descriptor.Rid == AcceptingNodeRid
                                        ? descriptor
                                        : descriptor with
                                        {
                                            PlacementWeight = 0,
                                        },
                                    "entry-not-accepted" => descriptor with
                                    {
                                        ObjectCapabilities = descriptor
                                            .ObjectCapabilities.Select(capability =>
                                                capability with
                                                {
                                                    EntryAccepted = false,
                                                }
                                            )
                                            .ToArray(),
                                    },
                                    "not-serving" => descriptor with
                                    {
                                        State = ZLinkFrameworkRuntimeState.Draining,
                                    },
                                    "wrong-type" => descriptor with
                                    {
                                        ObjectCapabilities = descriptor
                                            .ObjectCapabilities.Select(capability =>
                                                capability with
                                                {
                                                    StableType = "other",
                                                }
                                            )
                                            .ToArray(),
                                    },
                                    "wrong-kind" => descriptor with
                                    {
                                        ObjectCapabilities = descriptor
                                            .ObjectCapabilities.Select(capability =>
                                                capability with
                                                {
                                                    ObjectKind = ZLinkPlacementObjectKind.UserSpot,
                                                }
                                            )
                                            .ToArray(),
                                    },
                                    "client-role" => descriptor with
                                    {
                                        ObjectRole = ZLinkMeshNodeObjectRole.Client,
                                    },
                                    "no-entry" => descriptor with { EntrySpotId = null },
                                    "zero-weight" => descriptor with { PlacementWeight = 0 },
                                    "active-full" => descriptor with
                                    {
                                        Capacity = descriptor.Capacity with
                                        {
                                            Actors = new(1, 0, 1),
                                        },
                                    },
                                    "pending-full" => descriptor with
                                    {
                                        Capacity = descriptor.Capacity with
                                        {
                                            Actors = new(0, 1, 1),
                                        },
                                    },
                                    _ => throw new ArgumentOutOfRangeException(
                                        nameof(excludedCondition)
                                    ),
                                }
                            )
                            .ToArray()
            )
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            if (inUserSpot)
            {
                var created = await runtime.CreateAsync<EntryReturnSourceSpot>();
                var source = Assert.Single(
                    runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                    spot => spot.SpotId == created.Spot.SpotId
                );
                await source.JoinActorAsync(actor, ZLinkMessage.Empty, CancellationToken.None);
            }
            if (excludedCondition == "expired-owner")
            {
                var store = RequireLocationStore(runtime);
                var owner = await store.ClaimLiveOwnerAsync(
                    "expired-entry-owner",
                    TimeSpan.FromSeconds(1)
                );
                var descriptor = CreateNodeDescriptor(
                    runtime.Registration,
                    AcceptingNodeRid,
                    typeof(AcceptingProbeEntrySpot)
                ) with
                {
                    OwnerId = owner.OwnerId,
                    LeaseGeneration = owner.LeaseGeneration,
                };
                Assert.Equal(
                    ZLinkLocationWriteStatus.Stored,
                    (
                        await store.UpdateMeshNodeAsync(
                            descriptor,
                            ZLinkLocationWriteIntent.NewClaim
                        )
                    ).Status
                );
                time.Advance(TimeSpan.FromSeconds(2));
                Assert.Contains(
                    (await store.ListMeshNodesAsync("entry", new ZLinkPageRequest(100))).Items,
                    candidate => candidate.Rid == AcceptingNodeRid
                );
            }
            var previous = state.LiveActivation;
            var native = state.NativeActorRef;
            if (inUserSpot)
            {
                var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                    await runtime.JoinActorEntrySpotAsync(actor, ZLinkMessage.Empty, null)
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, exception.Kind);
            }
            else
            {
                // An Actor already in Entry has the separate no-op contract (§4).
                Assert.IsType<ZLinkActorJoinResult.Accepted>(
                    await runtime.JoinActorEntrySpotAsync(actor, ZLinkMessage.Empty, null)
                );
            }
            Assert.Same(previous, state.LiveActivation);
            Assert.Equal(native, state.NativeActorRef);
            Assert.Equal(0, node.NodeRequestAttempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(true, 2, 6)]
    [InlineData(false, 8, 0)]
    public async Task EntryJoinSelection_CurrentNodeHasNoPriorityAndUsesRelativeWeights(
        bool remoteAcceptsActor,
        int expectedLocal,
        int expectedRemote
    )
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(AcceptingProbeEntrySpot),
            userSpotType: typeof(EntryReturnSourceSpot),
            meshResolverWrapper: resolver => new EntryJoinDescriptors(
                resolver,
                descriptors =>
                {
                    var local = Assert.Single(descriptors);
                    return
                    [
                        local with
                        {
                            PlacementWeight = 1,
                        },
                        local with
                        {
                            Rid = AcceptingNodeRid,
                            PlacementWeight = 3,
                            ObjectCapabilities = local
                                .ObjectCapabilities.Select(capability =>
                                    capability with
                                    {
                                        EntryAccepted = remoteAcceptsActor,
                                    }
                                )
                                .ToArray(),
                        },
                    ];
                }
            )
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            var native = state.NativeActorRef;
            var created = await runtime.CreateAsync<EntryReturnSourceSpot>();
            var source = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                spot => spot.SpotId == created.Spot.SpotId
            );
            runtime.Registration.SpotNodes["entry"].ActorRelocations.Clear();
            var localSelections = 0;
            var remoteSelections = 0;
            for (var index = 0; index < 8; index++)
            {
                if (state.LiveActivation is null)
                    await source.JoinActorAsync(actor, ZLinkMessage.Empty, CancellationToken.None);
                try
                {
                    var result = await runtime.JoinActorEntrySpotAsync(
                        actor,
                        ZLinkMessage.Empty,
                        null
                    );
                    Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
                    Assert.Null(state.LiveActivation);
                    localSelections++;
                }
                catch (ZLinkFrameworkException exception)
                {
                    // This fixture disables relocation. Selecting the remote node must
                    // reach that policy rejection without changing the source Actor.
                    Assert.Equal(ZLinkFrameworkErrorKind.Rejected, exception.Kind);
                    Assert.Contains("relocation policy", exception.Message);
                    Assert.Same(source, state.LiveActivation);
                    remoteSelections++;
                }
                Assert.Equal(native, state.NativeActorRef);
            }
            Assert.Equal(expectedLocal, localSelections);
            Assert.Equal(expectedRemote, remoteSelections);
            Assert.Equal(0, node.NodeRequestAttempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntryJoinSelection_TargetRejectPreservesSourceWithoutSelectingAgain()
    {
        var replies = 0;
        CanonicalJoinSpotNode node = null!;
        node = new CanonicalJoinSpotNode
        {
            CanonicalActorJoinHandler = (callback, _) =>
            {
                replies++;
                callback(
                    new ZLinkBackendActorJoinResult(
                        RequestResult.Ok,
                        1,
                        node.CanonicalRequest.Actor with
                        {
                            NodeRid = AcceptingNodeRid,
                        },
                        node.CanonicalRequest.TargetSpotId,
                        0,
                        0,
                        JoinedSpotGeneration: node.CanonicalRequest.TargetSpotGeneration
                    ),
                    Array.Empty<Message>()
                );
            },
        };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(AcceptingProbeEntrySpot),
            userSpotType: typeof(EntryReturnSourceSpot),
            meshResolverWrapper: resolver => new EntryJoinDescriptors(
                resolver,
                descriptors =>
                    descriptors
                        .Select(descriptor =>
                            descriptor with
                            {
                                Rid = AcceptingNodeRid,
                                EntrySpotId = "remote-entry",
                            }
                        )
                        .ToArray()
            )
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            var native = state.NativeActorRef;
            var created = await runtime.CreateAsync<EntryReturnSourceSpot>();
            var source = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                spot => spot.SpotId == created.Spot.SpotId
            );
            await source.JoinActorAsync(actor, ZLinkMessage.Empty, CancellationToken.None);
            var previous = state.LiveActivation;
            var result = await runtime.JoinActorEntrySpotAsync(actor, ZLinkMessage.Empty, null);

            Assert.IsType<ZLinkActorJoinResult.Rejected>(result);
            Assert.Equal(1, replies);
            Assert.Equal(native, state.NativeActorRef);
            Assert.Same(previous, state.LiveActivation);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class EntryJoinDescriptors(
        IZLinkMeshNodeLocationResolver inner,
        Func<
            IReadOnlyList<ZLinkMeshNodeDescriptor>,
            IReadOnlyList<ZLinkMeshNodeDescriptor>
        > transform
    ) : IZLinkMeshNodeLocationResolver
    {
        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) => transform(await inner.ListLiveMeshNodesAsync(meshName, cancellationToken));
    }

    private static void AdmitAcceptingPeer(CapturingSpotNode node) =>
        node.AdmittedMeshPeers.Add(
            new MeshNodePeer(
                1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                AcceptingNodeRid,
                1,
                1,
                $"inproc://{AcceptingNodeRid}",
                1,
                0,
                1
            )
        );

    private static ZLinkMeshNodeDescriptor CreateNodeDescriptor(
        ZLinkFrameworkRegistration framework,
        RoutingId rid,
        Type entrySpotType
    )
    {
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "entry",
            RoutingId = rid,
            Router = new ZLinkSpotRouterCapabilityRegistration { BindEndpoint = $"inproc://{rid}" },
            EntrySpotType = entrySpotType,
        };
        new ZLinkMeshNodeBuilder(registration)
            .Objects()
            .Server()
            .AddActorFactory<ProbeActor, ProbeActorFactory>(
                "probe",
                factory => factory.DisableRelocation()
            );
        return ZLinkMeshNodeDescriptorFactory.Create(
            framework,
            registration,
            rid,
            lifecycleGeneration: 1,
            endpoint: $"inproc://{rid}",
            entrySpotId: $"entry:{rid}",
            descriptorRevision: 1,
            ZLinkFrameworkRuntimeState.Serving
        );
    }

    private static async Task<ZLinkMeshNodeDescriptor> PublishAcceptingNodeAsync(
        ZLinkFrameworkRuntime runtime,
        IZLinkLocationRepository store
    )
    {
        var owner = await store.ClaimLiveOwnerAsync("accepting-owner", TimeSpan.FromMinutes(1));
        var descriptor = CreateNodeDescriptor(
            runtime.Registration,
            AcceptingNodeRid,
            typeof(AcceptingProbeEntrySpot)
        ) with
        {
            OwnerId = owner.OwnerId,
            LeaseGeneration = owner.LeaseGeneration,
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(descriptor, ZLinkLocationWriteIntent.NewClaim)).Status
        );
        return descriptor;
    }

    //  The accepting node is remote: its target commits the reserved creation
    //  in the shared Location Store and answers Created.
    private static List<string> HandleRemoteCreatesOnAcceptingNode(
        CapturingSpotNode node,
        ZLinkFrameworkRuntime runtime,
        IZLinkLocationRepository store
    )
    {
        var creates = new List<string>();
        node.ActorCreateRemoteHandler = async (
            targetRid,
            actorId,
            stableType,
            fence,
            operation,
            deadlineUnixMs,
            _,
            _
        ) =>
        {
            Assert.Equal(AcceptingNodeRid, targetRid);
            lock (creates)
                creates.Add(actorId);
            var actor = new ActorRef(actorId, fence.ObjectGeneration, "entry", targetRid);
            var completed = await store.CompleteCreationAsync(
                new ZLinkObjectReservation(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                    fence.ExpectedStoreVersion,
                    fence.ObjectGeneration,
                    fence.AuthorityOwnerGeneration,
                    fence.ReservationId,
                    new ZLinkMeshNodeDescriptorKey("entry", targetRid),
                    fence.TargetNodeGeneration,
                    new ZLinkLocationOwnerToken(
                        fence.TargetOwnerId,
                        fence.TargetOwnerLeaseGeneration
                    )
                ),
                new ZLinkObjectCreationCompletion.Created(
                    ZLinkActorAuthorityPayloadCodec.Encode(
                        new ZLinkActorAuthorityPayload(
                            ZLinkActorAuthorityState.Ready,
                            stableType,
                            actorId,
                            $"entry:{targetRid}",
                            1,
                            ZLinkSpotKind.Entry,
                            fence.TargetOwnerId,
                            fence.TargetOwnerLeaseGeneration,
                            "entry",
                            targetRid,
                            fence.TargetNodeGeneration
                        )
                    ),
                    new ZLinkCreationTerminalPublication(
                        operation,
                        ZLinkActorCreationTerminalCodec.Encode(
                            new ActorCreateOperationTerminal(
                                RequestResult.Ok,
                                ServiceWireConstants.FrameworkErrorCode.None,
                                new ActorCreateCompletion(ActorCreateResult.Created, actor)
                            ),
                            runtime.Registration.Codecs
                        ),
                        DateTimeOffset
                            .FromUnixTimeMilliseconds(checked((long)deadlineUnixMs))
                            .AddMinutes(1)
                    )
                )
            );
            Assert.IsType<ZLinkObjectCreationCompleteResult.Created>(completed);
            return (
                new ActorCreateCompletion(ActorCreateResult.Created, actor),
                Array.Empty<Message>()
            );
        };
        return creates;
    }

    private sealed class AcceptingProbeEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<ProbeActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }
}
