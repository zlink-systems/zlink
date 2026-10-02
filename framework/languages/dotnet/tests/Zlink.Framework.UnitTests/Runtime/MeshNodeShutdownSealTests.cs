using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

// Spec 30 §14 step 1 (D-097): the mesh node follows the host's shutdown
// admission seal. After the seal it neither starts nor accepts peer admission,
// already-admitted peers still receive the Draining Update, and peer loss
// (transport disconnect, liveness expiry, failed control send) never undoes
// the published Draining. Service-wire §5: a repeated Hello/Admit that carries
// the current descriptor is idempotent - it completes the admission again
// without re-admitting the peer or resetting its descriptor/liveness epoch.
public sealed class MeshNodeShutdownSealTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private const string MeshName = "orders";
    private const string EphemeralTcpEndpoint = "tcp://127.0.0.1:0";

    [Fact]
    public void DrainGate_SealedForShutdown_OnlyByTheShutdownOwner()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.False(gate.IsSealedForShutdown);

        // A relocation fence seals application admission but is not a shutdown.
        Assert.True(
            gate.TryBeginRelocationFence(_ =>
            {
                gate.Seal();
                return true;
            })
        );
        Assert.True(gate.IsSealed);
        Assert.False(gate.IsSealedForShutdown);

        gate.ClaimShutdown();
        Assert.True(gate.IsSealedForShutdown);

        gate.Reset();
        Assert.False(gate.IsSealedForShutdown);
    }

    [Fact]
    public async Task CrossedHelloAdmit_CompletesOnceOnBothSides()
    {
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        await using var right = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        var suffix = Guid.NewGuid().ToString("N");
        var leftRid = RoutingId.From($"crossed-left-{suffix}");
        var rightRid = RoutingId.From($"crossed-right-{suffix}");
        left.SetRoutingId(leftRid);
        left.SetBind(EphemeralTcpEndpoint);
        left.AddChannel(MeshName);
        right.SetRoutingId(rightRid);
        right.SetBind(EphemeralTcpEndpoint);
        right.AddChannel(MeshName);
        using var leftMonitor = left.OpenMonitor();
        using var rightMonitor = right.OpenMonitor();

        // Both sides connect and announce, so each Hello is accepted and the
        // Admit that answers it arrives after the receiver already admitted
        // the same descriptor (the ZoneWorld E5 restart shape).
        try
        {
            // The actual sockets choose and hold their ports. Keep both Hello
            // submissions queued until both outbound intents are configured.
            left.Start();
            right.Start();
            var leftEndpoint = left.Status().LocalEndpoint;
            var rightEndpoint = right.Status().LocalEndpoint;
            Assert.NotEqual(leftEndpoint, rightEndpoint);
            left.ConnectPeer(rightEndpoint, rightRid);
            right.ConnectPeer(leftEndpoint, leftRid);
        }
        finally
        {
            scheduler.Release();
        }
        (
            IMeshNodeMonitor Monitor,
            RoutingId PeerRid,
            MeshNodeStatus Status,
            MeshNodePeer[] Peers
        )[] observations = [];
        await WaitUntilAsync(() =>
        {
            observations = new[]
            {
                (
                    Monitor: leftMonitor,
                    PeerRid: rightRid,
                    Status: left.Status(),
                    Peers: left.Peers()
                ),
                (
                    Monitor: rightMonitor,
                    PeerRid: leftRid,
                    Status: right.Status(),
                    Peers: right.Peers()
                ),
            };
            return observations.All(observation =>
                observation.Status.State == MeshNodeState.Ready
                && observation.Status.AdmittedPeerCount == 1
                && observation.Peers.Length == 1
                && observation.Peers[0].RoutingId == observation.PeerRid
                && observation.Peers[0].State == MeshPeerState.Admitted
            );
        });
        // Preserve both nodes' complete event order before an assertion can fail.
        (MeshMonitorStatus Status, List<MeshMonitorEvent> Events) ObserveMonitor(
            IMeshNodeMonitor monitor,
            RoutingId peerRid,
            string phase
        )
        {
            var events = new List<MeshMonitorEvent>();
            while (monitor.Recv(RecvFlags.DontWait) is { } meshEvent)
                events.Add(meshEvent);
            var status = monitor.Status();
            output.WriteLine(
                "{0} monitor peer={1} status={2} events=[{3}]",
                phase,
                peerRid,
                System.Text.Json.JsonSerializer.Serialize(status),
                string.Join(
                    ", ",
                    events.Select(meshEvent => $"{meshEvent.Kind}:{meshEvent.PeerRid}")
                )
            );
            return (status, events);
        }
        var monitorObservations = observations
            .Select(observation =>
                (
                    Observation: observation,
                    Snapshot: ObserveMonitor(observation.Monitor, observation.PeerRid, "Initial")
                )
            )
            .ToArray();
        foreach (var (observation, (status, events)) in monitorObservations)
        {
            var (_, peerRid, nodeStatus, peers) = observation;
            Assert.Single(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind == MeshMonitorEventKind.PeerAdmitted
            );
            Assert.DoesNotContain(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind == MeshMonitorEventKind.PeerClosed
            );
            Assert.Equal(1UL, status.PeerAdmitted);
            Assert.Equal(0UL, status.ProtocolErrors);
            Assert.Equal(MeshNodeState.Ready, nodeStatus.State);
            Assert.Equal(1U, nodeStatus.AdmittedPeerCount);
            var admitted = Assert.Single(peers, peer => peer.RoutingId == peerRid);
            Assert.Equal(MeshPeerState.Admitted, admitted.State);
        }
        var finalObservations = monitorObservations
            .Select(first =>
                (
                    PeerRid: first.Observation.PeerRid,
                    InitialStatus: first.Snapshot.Status,
                    Snapshot: ObserveMonitor(
                        first.Observation.Monitor,
                        first.Observation.PeerRid,
                        "Final"
                    )
                )
            )
            .ToArray();
        foreach (var (peerRid, initialStatus, (status, events)) in finalObservations)
        {
            Assert.DoesNotContain(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind
                        is MeshMonitorEventKind.PeerAdmitted
                            or MeshMonitorEventKind.PeerClosed
            );
            Assert.Equal(initialStatus.PeerAdmitted, status.PeerAdmitted);
            Assert.Equal(initialStatus.ProtocolErrors, status.ProtocolErrors);
            Assert.Equal(initialStatus.PeerRejected, status.PeerRejected);
        }
    }

    [Fact]
    public async Task InboundAdmissionBeforeEndpointIntent_KeepsOnePeerForTheRid()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(context, MeshName);
        await using var right = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var leftRid = RoutingId.From($"inbound-left-{suffix}");
        var rightRid = RoutingId.From($"inbound-right-{suffix}");
        left.SetRoutingId(leftRid);
        left.SetBind(EphemeralTcpEndpoint);
        left.AddChannel(MeshName);
        right.SetRoutingId(rightRid);
        right.SetBind(EphemeralTcpEndpoint);
        right.AddChannel(MeshName);
        left.Start();
        right.Start();
        right.ConnectPeer(left.Status().LocalEndpoint, leftRid);
        await WaitUntilAsync(() =>
            left.Status().AdmittedPeerCount == 1 && right.Status().AdmittedPeerCount == 1
        );

        var intent = left.ConnectPeer(right.Status().LocalEndpoint);
        var peer = Assert.Single(left.Peers());
        Assert.Equal(rightRid, peer.RoutingId);
        Assert.Equal(intent, peer.ConnectionIntentId);
        Assert.Equal(MeshPeerState.Admitted, peer.State);

        left.RemovePeerConnection(intent);
        Assert.Equal(0U, left.Status().AdmittedPeerCount);
        Assert.DoesNotContain(left.Peers(), candidate => candidate.ConnectionIntentId == intent);
        Assert.DoesNotContain(left.Peers(), candidate => candidate.RoutingId == rightRid);
    }

    [Fact]
    public async Task FailedAdmitSend_KeepsConfiguredNotRequiredPeer()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("not-required-send-failure");
        const ulong routeGeneration = 17;
        node.ConnectPeer("tcp://127.0.0.1:20002", rid);
        var peer = new ZLinkMeshPeer(999)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            RouteGeneration = routeGeneration,
            State = MeshPeerState.NotRequired,
        };
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(rid, peer);
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ClosePeerAfterControlSendFailure", Private)!
            .Invoke(node, [rid, routeGeneration]);

        Assert.Equal(MeshPeerState.NotRequired, peer.State);
        Assert.True(index.ContainsKey(rid));
        Assert.Equal(0UL, peer.RouteGeneration);
    }

    [Fact]
    public async Task NewHandshakeRoute_FencesControlQueuedForOldSnapshot()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("handover-control-peer");
        node.ConnectPeer("tcp://127.0.0.1:20004", rid);
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        routes.Apply([new RouterRoute(rid, 11)]);
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(rid, new ZLinkMeshPeer(999) { RoutingId = rid, RouteGeneration = 12 });
        var hasTarget = typeof(ZLinkManagedMeshNode).GetMethod("HasCurrentControlTarget", Private)!;

        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 11UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.True((bool)hasTarget.Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Admit])!);
    }

    [Fact]
    public async Task RejectedNewHandshake_FencesOldControlWithoutSnapshotUpdate()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("rejected-handover-peer");
        const string endpoint = "tcp://127.0.0.1:20005";
        node.ConnectPeer(endpoint, rid);
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        routes.Apply([new RouterRoute(rid, 11)]);
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(
            rid,
            new ZLinkMeshPeer(999)
            {
                RoutingId = rid,
                PhysicalRoutingId = rid,
                RouteGeneration = 11,
                Admitted = true,
                State = MeshPeerState.Admitted,
            }
        );
        var wrongMesh = new ZLinkServiceWireCodec.AdmissionRecord(
            "different-mesh",
            ZLinkServiceSecurityIdentity.Plaintext,
            endpoint,
            7,
            1,
            new Dictionary<string, uint>(),
            1,
            1,
            (byte)ZLinkMeshNodeObjectRole.Server,
            100,
            0,
            0,
            0,
            0,
            new Dictionary<byte, byte[]>(),
            []
        );
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ProcessAdmissionCore", Private)!
            .Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Hello, wrongMesh, null]);

        var hasTarget = typeof(ZLinkManagedMeshNode).GetMethod("HasCurrentControlTarget", Private)!;
        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 11UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.True(index.TryGetValue(rid, out var disconnected));
        Assert.Equal(0UL, disconnected.RouteGeneration);
        Assert.False(disconnected.Admitted);
    }

    [Fact]
    public async Task SameRid_DifferentEndpointIntent_AdmitsTheConnectedEndpoint()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        await using var remote = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var remoteRid = RoutingId.From($"intent-remote-{suffix}");
        owner.SetRoutingId(RoutingId.From($"intent-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        remote.SetRoutingId(remoteRid);
        remote.SetBind(EphemeralTcpEndpoint);
        remote.AddChannel(MeshName);
        remote.Start();
        owner.Start();

        var obsoleteIntent = owner.ConnectPeer("tcp://127.0.0.1:1", remoteRid);
        owner.ConnectPeer(remote.Status().LocalEndpoint, remoteRid);
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        owner.RemovePeerConnection(obsoleteIntent);
        Assert.Equal(1U, owner.Status().AdmittedPeerCount);
    }

    [Fact]
    public async Task SameRid_SameEndpoint_UsesMatchingSecurityIntent()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        await using var remote = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var remoteRid = RoutingId.From($"security-remote-{suffix}");
        owner.SetRoutingId(RoutingId.From($"security-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        remote.SetRoutingId(remoteRid);
        remote.SetBind(EphemeralTcpEndpoint);
        remote.AddChannel(MeshName);
        remote.Start();
        var endpoint = remote.Status().LocalEndpoint;
        owner.ConnectPeer(endpoint, remoteRid, "unavailable-security-identity");
        owner.ConnectPeer(endpoint, remoteRid);
        owner.Start();

        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        Assert.Equal(
            remoteRid,
            Assert
                .Single(owner.Peers().Where(peer => peer.State == MeshPeerState.Admitted))
                .RoutingId
        );
    }

    [Fact]
    public async Task RepeatedEndpointConfiguration_ReusesOnePendingIntent()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("pending-intent-peer");
        const string endpoint = "tcp://127.0.0.1:20003";

        var first = node.ConnectPeer(endpoint, rid);
        var repeated = node.ConnectPeer(endpoint, rid);

        Assert.Equal(first, repeated);
        Assert.Equal(first, Assert.Single(node.Peers()).ConnectionIntentId);
    }

    [Fact]
    public async Task EndpointIntent_LearnsReplacementRidAfterRouteLoss()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        owner.SetRoutingId(RoutingId.From($"replacement-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        owner.Start();

        var firstRid = RoutingId.From($"replacement-first-{suffix}");
        var first = new ZLinkManagedMeshNode(context, MeshName);
        first.SetRoutingId(firstRid);
        first.SetBind(EphemeralTcpEndpoint);
        first.AddChannel(MeshName);
        first.Start();
        var endpoint = first.Status().LocalEndpoint;
        owner.ConnectPeer(endpoint);
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        await first.DisposeAsync();
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 0);

        await using var replacement = new ZLinkManagedMeshNode(context, MeshName);
        var replacementRid = RoutingId.From($"replacement-second-{suffix}");
        replacement.SetRoutingId(replacementRid);
        replacement.SetBind(endpoint);
        replacement.AddChannel(MeshName);
        replacement.Start();
        await WaitUntilAsync(() =>
            owner
                .Peers()
                .Any(peer =>
                    peer.RoutingId == replacementRid && peer.State == MeshPeerState.Admitted
                )
        );
        Assert.DoesNotContain(owner.Peers(), peer => peer.RoutingId == firstRid);
    }

    [Fact]
    public async Task HelloBeforeRouteObservation_AdmitIsSentOnTheRecordRoute()
    {
        // Core ROUTER §10.1: a record carries the generation of the route
        // that delivered it. The receive loop can dispatch a Hello before its
        // snapshot observes that route; the admission epoch is the record's
        // route, so the observation arriving later must not fence the Admit.
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"record-route-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        var peerRid = RoutingId.From($"record-route-peer-{suffix}");
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        var lane = (Zlink.Framework.Runtime.Execution.ZLinkStateLane)
            typeof(ZLinkManagedMeshNode).GetField("_lane", Private)!.GetValue(node)!;
        try
        {
            node.Start();
            using var peer = context.CreateDealerSocket();
            peer.SetRoutingId(peerRid);
            peer.Connect(node.Status().LocalEndpoint);
            await WaitUntilAsync(() => routes.GenerationOf(peerRid) != 0);
            var generation = routes.GenerationOf(peerRid);

            // The snapshot has not observed the route yet.
            await lane.RunAsync(routes.Clear);
            await SendAsync(
                peer,
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Hello,
                    MeshName,
                    $"inproc://record-route-peer-{suffix}",
                    lifecycleGeneration: 7,
                    descriptorRevision: 3,
                    new Dictionary<string, uint>(StringComparer.Ordinal),
                    objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                )
            );
            await scheduler.Queued;
            await WaitUntilAsync(() =>
                node.Peers().Any(candidate => candidate.RoutingId == peerRid)
            );

            scheduler.Release();

            await ReceiveAdmitAsync(peer);
            await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
            await lane.RunAsync(() => routes.Apply([new RouterRoute(peerRid, generation)]));
        }
        finally
        {
            scheduler.Release();
        }
    }

    [Fact]
    public async Task IdempotentAdmit_CompletesWithoutReadmittingOrResettingTheEpoch()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"idempotent-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;

        var peerRid = RoutingId.From($"idempotent-peer-{suffix}");
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(peerRid);
        peer.Connect(endpoint);
        byte[] Descriptor(ServiceWireConstants.Command command) =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                MeshName,
                $"inproc://idempotent-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            );

        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
        await ReceiveAdmitAsync(peer);
        var admitted = Assert.Single(node.Peers());
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);

        // The same descriptor again, first as Admit then as Hello: both are
        // idempotent completions. The Hello still gets its Admit reply; the
        // peer is neither re-admitted nor moved to a new epoch.
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Admit));
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        await ReceiveAdmitAsync(peer);

        var status = monitor.Status();
        Assert.Equal(1UL, status.PeerAdmitted);
        Assert.Equal(0UL, status.PeerRejected);
        Assert.Equal(0UL, status.ProtocolErrors);
        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        var current = Assert.Single(node.Peers());
        Assert.Equal(MeshPeerState.Admitted, current.State);
        Assert.Equal(admitted.LifecycleGeneration, current.LifecycleGeneration);
        Assert.Equal(admitted.DescriptorRevision, current.DescriptorRevision);
        Assert.Equal(admitted.LastChangedMs, current.LastChangedMs);
    }

    [Fact]
    public async Task SameConnectionHello_WithoutPendingCandidate_PreservesEpochAndLivenessDeadline()
    {
        DeferredReadyMonitor? transportMonitor = null;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            decorateSocketMonitor: monitor => transportMonitor = new DeferredReadyMonitor(monitor)
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"same-connection-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"same-connection-peer-{suffix}"));

        transportMonitor!.ReleaseReady();
        peer.Connect(endpoint);
        await transportMonitor.ReadyCaptured;
        await transportMonitor.ReadyApplied;

        byte[] Hello() =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                MeshName,
                $"inproc://same-connection-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            );

        await SendAsync(peer, Hello());
        await ReceiveAdmitAsync(peer);
        var admitted = Assert.Single(node.Peers());

        // The first Hello consumed the only READY candidate. The same physical
        // connection has no pending replacement candidate, so this Hello keeps
        // both the connection epoch and its liveness deadline.
        await SendAsync(peer, Hello());
        await ReceiveAdmitAsync(peer);

        Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        Assert.Equal(0UL, monitor.Status().PeerRejected);
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        var current = Assert.Single(node.Peers());
        Assert.Equal(admitted.LifecycleGeneration, current.LifecycleGeneration);
        Assert.Equal(admitted.DescriptorRevision, current.DescriptorRevision);
        Assert.Equal(admitted.LastChangedMs, current.LastChangedMs);
    }

    [Fact]
    public async Task QueuedAdmit_SurvivesConnectionReadyDeliveredAfterHello()
    {
        var scheduler = new GatedTaskScheduler();
        DeferredReadyMonitor? transportMonitor = null;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler,
            decorateSocketMonitor: monitor => transportMonitor = new DeferredReadyMonitor(monitor)
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"late-ready-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"late-ready-peer-{suffix}"));
        peer.Connect(endpoint);

        try
        {
            using (
                var hello = Message.From(
                    ZLinkServiceWireCodec.EncodeRouteAdmission(
                        ServiceWireConstants.Command.Hello,
                        MeshName,
                        $"inproc://late-ready-peer-{suffix}",
                        lifecycleGeneration: 7,
                        descriptorRevision: 3,
                        new Dictionary<string, uint>(StringComparer.Ordinal),
                        objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                    )
                )
            )
                peer.Send().Message(hello).Submit();

            // Hello validates ingress, but a queued Admit is not a ready route.
            // A late transport READY must preserve this pending handshake.
            await scheduler.Queued;
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            var pending = Assert.Single(node.Peers());
            await transportMonitor!.ReadyCaptured;
            transportMonitor.ReleaseReady();
            await transportMonitor.ReadyApplied;
            var afterReady = Assert.Single(node.Peers());
            Assert.Equal(pending.LifecycleGeneration, afterReady.LifecycleGeneration);
            Assert.Equal(pending.DescriptorRevision, afterReady.DescriptorRevision);
            Assert.Equal(pending.LastChangedMs, afterReady.LastChangedMs);
            Assert.Equal(0U, node.Status().AdmittedPeerCount);

            // Update follows the queued Admit on the same socket. It makes a
            // discarded Admit observable as the wrong first record, without a
            // receive deadline or waiting for an absent message.
            node.PublishDraining();
            scheduler.Release();
            foreach (
                var expected in new[]
                {
                    ServiceWireConstants.Command.Admit,
                    ServiceWireConstants.Command.Update,
                }
            )
            {
                using var received = Received.Create();
                Assert.True(peer.Recv(received));
                Assert.True(
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                );
                Assert.Equal(expected, command);
            }

            Assert.True(
                SpinWait.SpinUntil(
                    () => node.Status().AdmittedPeerCount == 1,
                    TimeSpan.FromSeconds(5)
                )
            );
            Assert.Equal(1UL, monitor.Status().PeerAdmitted);
            Assert.Equal(0UL, monitor.Status().PeerRejected);
            Assert.Equal(0UL, monitor.Status().ProtocolErrors);
            var current = Assert.Single(node.Peers());
            Assert.Equal(pending.LifecycleGeneration, current.LifecycleGeneration);
            Assert.Equal(pending.DescriptorRevision, current.DescriptorRevision);
        }
        finally
        {
            transportMonitor?.ReleaseReady();
            scheduler.Release();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task InboundHello_FollowsTheHostShutdownSeal(bool sealedForShutdown)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var gate = new ZLinkDrainAdmissionGate();
        var sealObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        node.SetPeerAdmissionSealGate(() =>
        {
            sealObserved.TrySetResult();
            return gate.IsSealedForShutdown;
        });
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"inbound-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        if (sealedForShutdown)
        {
            gate.ClaimShutdown();
            node.PublishDraining();
        }
        var state = node.Status().State;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"inbound-peer-{suffix}"));
        peer.Connect(endpoint);

        await SendAsync(
            peer,
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                MeshName,
                $"inproc://inbound-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            )
        );
        // With no outbound intent, only the inbound Hello queries this gate.
        await sealObserved.Task.WaitAsync(TimeSpan.FromSeconds(2));

        if (sealedForShutdown)
        {
            Assert.Equal(state, node.Status().State);
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            Assert.Empty(node.Peers());
            Assert.Equal(0UL, monitor.Status().PeerAdmitted);
            Assert.Equal(0UL, monitor.Status().PeerRejected);
            await AssertNoAdmitAsync(peer);
        }
        else
        {
            await ReceiveAdmitAsync(peer);
            Assert.Equal(1U, node.Status().AdmittedPeerCount);
            Assert.Equal(MeshPeerState.Admitted, Assert.Single(node.Peers()).State);
            Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        }
    }

    [Fact]
    public async Task SealedNode_IgnoresRepeatedHello_ButProcessesAdmittedPeerUpdate()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var gate = new ZLinkDrainAdmissionGate();
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"update-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"update-peer-{suffix}"));
        peer.Connect(endpoint);
        byte[] Descriptor(ServiceWireConstants.Command command, ulong revision, byte state) =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                MeshName,
                $"inproc://update-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: revision,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server,
                runtimeState: state
            );

        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello, 3, 1));
        await ReceiveAdmitAsync(peer);
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        gate.ClaimShutdown();
        node.PublishDraining();

        // If the sealed Hello mutates the descriptor to revision 99, the
        // following revision-4 Update cannot be accepted as the next revision.
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello, 99, 1));
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Update, 4, 2));
        await WaitUntilAsync(() =>
            node.Peers()
                .Any(remote =>
                    remote.DescriptorRevision == 4 && remote.State == MeshPeerState.Draining
                )
        );

        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        Assert.Equal(MeshNodeState.Draining, node.Status().State);
        Assert.Equal(0UL, monitor.Status().PeerRejected);
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        await AssertNoAdmitAsync(peer);
    }

    [Fact]
    public async Task SealedNode_StartsNoPeerAdmissionAfterPeerLoss_AndKeepsDraining()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"sealed-peer-{suffix}");
        string peerEndpoint;
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"sealed-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);

        await using (var peer = StartPeer(context, peerRid, EphemeralTcpEndpoint))
        {
            peerEndpoint = peer.Status().LocalEndpoint;
            node.ConnectPeer(peerEndpoint, peerRid);
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
            );

            // ZLinkFrameworkRuntime shutdown order: seal host admission, then
            // publish Draining. The admitted peer still receives that Update.
            gate.ClaimShutdown();
            node.PublishDraining();
            await WaitUntilAsync(() =>
                peer.Peers()
                    .Any(remote =>
                        remote.RoutingId == node.RoutingId && remote.State == MeshPeerState.Draining
                    )
            );
        }

        // Peer loss demotes the outbound intent to a reconnecting epoch but
        // never moves the sealed node back before Draining.
        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting)
        );
        Assert.Equal(MeshNodeState.Draining, node.Status().State);

        // The peer restarts at the same endpoint. Core reconnects the pipe,
        // but the sealed node submits no Hello, so the restarted peer never
        // learns about it.
        await using (var restarted = StartPeer(context, peerRid, peerEndpoint))
        {
            await Task.Delay(TimeSpan.FromSeconds(2));
            Assert.Equal(0U, restarted.Status().AdmittedPeerCount);
            Assert.Empty(restarted.Peers());
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            Assert.Equal(MeshNodeState.Draining, node.Status().State);
            Assert.Equal(MeshPeerState.Connecting, Assert.Single(node.Peers()).State);

            // Drain completes: the node stops without waiting on the withheld
            // admission.
            await node.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(10));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task UnsealedNode_ReadmitsRestartedPeer(bool relocationDraining)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"unsealed-peer-{suffix}");
        string peerEndpoint;
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"unsealed-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);

        await using (var peer = StartPeer(context, peerRid, EphemeralTcpEndpoint))
        {
            peerEndpoint = peer.Status().LocalEndpoint;
            node.ConnectPeer(peerEndpoint, peerRid);
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
            );
            if (relocationDraining)
            {
                // Relocate publishes Draining without sealing the host: the
                // node keeps admitting peers.
                node.PublishDraining();
                await WaitUntilAsync(() =>
                    peer.Peers()
                        .Any(remote =>
                            remote.RoutingId == node.RoutingId
                            && remote.State == MeshPeerState.Draining
                        )
                );
            }
        }

        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting)
        );

        await using var restarted = StartPeer(context, peerRid, peerEndpoint);
        await WaitUntilAsync(
            () => node.Status().AdmittedPeerCount == 1 && restarted.Status().AdmittedPeerCount == 1,
            TimeSpan.FromSeconds(15)
        );
        Assert.False(gate.IsSealedForShutdown);
        Assert.Equal(
            relocationDraining ? MeshNodeState.Draining : MeshNodeState.Ready,
            node.Status().State
        );
        Assert.Equal(
            relocationDraining ? MeshPeerState.Draining : MeshPeerState.Admitted,
            Assert.Single(restarted.Peers()).State
        );
    }

    private sealed class DeferredReadyMonitor(ISocketMonitor inner) : ISocketMonitor
    {
        private readonly Queue<MonitorEvent> _ready = new();
        private readonly TaskCompletionSource _captured = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource _applied = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _release;
        private bool _delivered;

        internal Task ReadyCaptured => _captured.Task;
        internal Task ReadyApplied => _applied.Task;

        internal void ReleaseReady()
        {
            Volatile.Write(ref _release, 1);
        }

        public MonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
        {
            // The owner calls Recv again only after handling the prior event.
            if (_delivered)
            {
                _delivered = false;
                _applied.TrySetResult();
            }
            if (Volatile.Read(ref _release) != 0 && _ready.TryDequeue(out var ready))
            {
                _delivered = true;
                return ready;
            }
            while (inner.Recv(flags) is { } value)
            {
                if (
                    value.Event == MonitorEventType.ConnectionReady
                    && (value.Flags & MonitorEventFlags.ConnectionReadyEdge) != 0
                )
                {
                    _ready.Enqueue(value);
                    _captured.TrySetResult();
                    continue;
                }
                return value;
            }
            return null;
        }

        public MonitorStatus Status() => inner.Status();

        public void Close() => inner.Close();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }

    private static ZLinkManagedMeshNode StartPeer(
        IContext context,
        RoutingId routingId,
        string endpoint
    )
    {
        var peer = new ZLinkManagedMeshNode(context, MeshName);
        peer.SetRoutingId(routingId);
        peer.SetBind(endpoint);
        peer.AddChannel(MeshName);
        peer.Start();
        return peer;
    }

    private static async Task SendAsync(IDealerSocket socket, byte[] head)
    {
        var deadlineTimeout = TimeSpan.FromSeconds(2);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (true)
        {
            try
            {
                using var message = Message.From(head);
                await socket.Send().Message(message).Async(CancellationToken.None).Admitted;
                return;
            }
            catch (ZlinkSubmitException)
                when (Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
            {
                await Task.Delay(10);
            }
        }
    }

    // Skips liveness probes; returns once a route Admit arrives.
    private static async Task ReceiveAdmitAsync(IDealerSocket socket)
    {
        var deadlineTimeout = TimeSpan.FromSeconds(2);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
        {
            using var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                if (
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                    && command == ServiceWireConstants.Command.Admit
                )
                    return;
                continue;
            }
            await Task.Delay(10);
        }
        throw new TimeoutException("The route Admit reply was not received.");
    }

    private static async Task AssertNoAdmitAsync(IDealerSocket socket)
    {
        var started = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(started) < TimeSpan.FromMilliseconds(200))
        {
            using var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                if (
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                )
                    Assert.NotEqual(ServiceWireConstants.Command.Admit, command);
                continue;
            }
            await Task.Delay(10);
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan? timeout = null)
    {
        var deadlineTimeout = timeout ?? TimeSpan.FromSeconds(5);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!condition())
        {
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= deadlineTimeout)
                throw new TimeoutException("The managed MeshNode condition was not reached.");
            await Task.Delay(10);
        }
    }
}
