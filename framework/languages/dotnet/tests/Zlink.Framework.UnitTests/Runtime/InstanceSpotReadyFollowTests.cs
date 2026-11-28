using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class InstanceSpotReadyFollowTests
{
    [Theory]
    [InlineData("follow")]
    [InlineData("committed-staging")]
    [InlineData("follow-without-intent")]
    [InlineData("direct")]
    [InlineData("no-route")]
    [InlineData("expired")]
    [InlineData("generation-mismatch")]
    [InlineData("hop-limit")]
    public async Task Ready_send_after_relocation_uses_the_expected_route(string scenario)
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            $"ready-follow-{scenario}-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        var roots = new SynchronizedRelocationStore();
        await using var previous = await SpotCloseHost.StartAsync(
            relocationStore: roots,
            relocateInstances: true
        );
        var duration = scenario switch
        {
            "no-route" => TimeSpan.Zero,
            "expired" => TimeSpan.FromSeconds(1),
            _ => TimeSpan.FromSeconds(30),
        };
        var locations = previous.Runtime.Registration.Locations.Options;
        locations.RouteCacheMaxAge = TimeSpan.Zero;
        locations.MessageFollowDuration = duration;
        const string spotId = "relocated-ready-send";
        const string marker = "sent-after-relocation";
        var initial = await previous.RequestInstanceAsync(spotId);
        var previousRuntime = previous.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName);
        var previousNode = Assert
            .IsType<ZLinkBackendSpotNodeWrapper>(previousRuntime.Node)
            .NativeNode;
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await previous
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        var snapshot = new ZLinkSpotHandleSnapshot(
            SpotCloseHost.MeshName,
            previousNode.RoutingId,
            spotId,
            initial.Generation,
            ZLinkSpotKind.Instance,
            authority.AuthorityOwnerGeneration,
            previousNode.Status().LifecycleGeneration,
            checked((ulong)authority.OwnerLeaseGeneration),
            authority.OwnerId,
            authority.StoreVersion
        );
        await using var target = await SpotCloseHost.StartAsync(
            previous.Store,
            relocationStore: roots,
            relocateInstances: true
        );
        await using var caller = await SpotCloseHost.StartAsync(previous.Store, clientOnly: true);
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await previous.WaitForPeerAsync(target, deadline.Token);
        await caller.WaitForPeerAsync(scenario == "direct" ? target : previous, deadline.Token);
        var holdPublication = scenario == "committed-staging";
        var targetNode = target.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node;
        var prepared = new TaskCompletionSource<ZLinkInstanceSpotActivation>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        if (holdPublication)
        {
            target.State.OnInstanceConstructed = context =>
                prepared.TrySetResult(Assert.IsType<ZLinkInstanceSpotActivation>(context));
            previous.Store.HoldAfterPut = true;
            previous.Store.HoldAfterAggregateCommitForOwner = targetNode.RoutingId;
            previous.Store.HoldNextPutFor = spotId;
        }

        var relocation = previousRuntime
            .Catalog.TryRelocateForRetireAsync(
                new ZLinkRelocationTargetSelection(
                    ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    target.Runtime.Registration.ApplicationVersion
                ),
                ZLinkSpotRelocationPhase.Aggregates,
                deadline.Token
            )
            .AsTask();
        if (holdPublication)
        {
            try
            {
                await previous.Store.PutHeld.Task.WaitAsync(deadline.Token);
                var staged = await prepared.Task.WaitAsync(deadline.Token);
                Assert.True(staged.PendingApplicationCompletion.IsCompleted);
                Assert.Equal(1UL, (await relocation.WaitAsync(deadline.Token)).CommittedUnitCount);

                await SubmitReadyAsync(snapshot);
                await WaitUntilAsync(
                    () =>
                        !staged.PendingApplicationCompletion.IsCompleted || FindDrops().Length != 0,
                    deadline.Token
                );
                Assert.Empty(FindDrops());
                Assert.False(staged.PendingApplicationCompletion.IsCompleted);
                Assert.DoesNotContain(marker, previous.State.InstanceHandlerMarkers);
                Assert.DoesNotContain(marker, target.State.InstanceHandlerMarkers);

                var committed = await ReadCommittedRouteAsync();
                var targetEndpoint = Assert.IsType<string>(
                    Assert
                        .IsType<ZLinkBackendSpotNodeWrapper>(targetNode)
                        .NativeNode.Status()
                        .LocalEndpoint
                );
                await SendFollowAsync(
                    targetEndpoint,
                    committed with
                    {
                        AuthorityOwnerGeneration = committed.AuthorityOwnerGeneration + 1,
                    },
                    hop: 1,
                    "wrong-fence"
                );
                await WaitUntilAsync(() => FindDrops().Length != 0, deadline.Token);
                Assert.Single(FindDrops());
                Assert.DoesNotContain(marker, target.State.InstanceHandlerMarkers);
            }
            finally
            {
                previous.Store.ReleasePut.TrySetResult();
            }
        }
        var relocated = await relocation;
        Assert.True(relocated.Completed, relocated.ToString());
        Assert.Equal(1UL, relocated.CommittedUnitCount);
        if (scenario == "expired")
        {
            // Measure the full monotonic Follow duration from the confirmed commit.
            var completedAt = Stopwatch.GetTimestamp();
            await WaitUntilAsync(
                () => Stopwatch.GetElapsedTime(completedAt) >= duration,
                deadline.Token
            );
        }

        if (scenario == "hop-limit")
        {
            await SendFollowAsync(
                Assert.IsType<string>(previousNode.Status().LocalEndpoint),
                snapshot,
                hop: 8,
                marker
            );
            await WaitUntilAsync(() => FindDrops().Length != 0, deadline.Token);
        }
        else if (!holdPublication)
        {
            var route = scenario switch
            {
                "direct" => await ReadCommittedRouteAsync(),
                "generation-mismatch" => snapshot with { Generation = snapshot.Generation + 1 },
                _ => snapshot,
            };
            await SubmitReadyAsync(route);
        }

        using var handlerDeadline = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        await WaitUntilAsync(
            () =>
                target.State.InstanceHandlerMarkers.Contains(marker)
                || FindDrops().Length > (holdPublication ? 1 : 0),
            handlerDeadline.Token
        );
        MessageFlowDeliveryTests.WaitForDelivery();
        if (scenario is "direct" or "follow" or "follow-without-intent" or "committed-staging")
        {
            Assert.True(
                target.State.InstanceHandlerMarkers.Contains(marker),
                "The relocated Instance Spot did not receive the Ready send."
            );
            Assert.Single(target.State.InstanceHandlerMarkers, value => value == marker);
            Assert.Equal(holdPublication ? 1 : 0, FindDrops().Length);
            Assert.DoesNotContain("wrong-fence", target.State.InstanceHandlerMarkers);
        }
        else
        {
            Assert.Single(FindDrops());
            Assert.DoesNotContain(marker, target.State.InstanceHandlerMarkers);
        }
        Assert.DoesNotContain(marker, previous.State.InstanceHandlerMarkers);

        async Task<ZLinkSpotHandleSnapshot> ReadCommittedRouteAsync()
        {
            var committed = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await target
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
                )
                .Snapshot;
            Assert.Equal(targetNode.RoutingId, committed.Allocation.Descriptor.Rid);
            Assert.NotEqual(snapshot.AuthorityOwnerGeneration, committed.AuthorityOwnerGeneration);
            Assert.Equal(snapshot.Generation, committed.ObjectGeneration);
            return snapshot with
            {
                NodeRid = committed.Allocation.Descriptor.Rid,
                NodeGeneration = committed.Allocation.DescriptorLifecycleGeneration,
                AuthorityOwnerGeneration = committed.AuthorityOwnerGeneration,
                OwnerLeaseGeneration = checked((ulong)committed.OwnerLeaseGeneration),
                OwnerId = committed.OwnerId,
                StoreVersion = committed.StoreVersion,
            };
        }

        async Task SubmitReadyAsync(ZLinkSpotHandleSnapshot route)
        {
            var handle = new ZLinkResolvedSpotHandle(
                route,
                1,
                _ =>
                    throw new InvalidOperationException(
                        "The test must submit the cached Ready route."
                    )
            );
            await new ZLinkRouteSpotSendCall<SpotCloseProbeSignal>(
                caller.Runtime,
                handle,
                new(marker),
                instanceIntent: scenario != "follow-without-intent"
            ).Async();
        }

        async Task SendFollowAsync(
            string endpoint,
            ZLinkSpotHandleSnapshot route,
            byte hop,
            string payloadMarker
        )
        {
            var callerNode = caller.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node;
            await using var context = Systems.Zlink.Zlink.CreateContext();
            using var forwarder = context.CreateDealerSocket();
            using var completion = new TestCompletionPollerDriver(forwarder);
            await AdmitForwarderAsync(forwarder, endpoint, SpotCloseHost.MeshName, deadline.Token);
            var follow = CreateFollow(
                callerNode.RoutingId,
                callerNode.MeshStatus().LifecycleGeneration,
                route,
                hop,
                hasMetadata: false
            );
            using var head = Message.From(
                ServiceWireCodec.EncodeInstanceSpotFollow55Header(
                    follow,
                    ServiceWireCodec.DecodeContext.Empty
                )
            );
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    TimeProvider.System,
                    ZLinkMessageKind.Command,
                    SpotCloseHost.MeshName,
                    nameof(SpotCloseProbeSignal)
                ),
                new SpotCloseProbeSignal(payloadMarker),
                caller.Runtime.Registration.Codecs
            );
            try
            {
                using var payload = Message.From(
                    ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(parts)
                );
                await forwarder.Send().Messages([head, payload]).Async(deadline.Token).Admitted;
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
        }

        string[] FindDrops() =>
            listener
                .ReadLines()
                .Where(line =>
                    line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal)
                    && line.Contains($"\"spot_id\":\"{spotId}\"", StringComparison.Ordinal)
                    && line.Contains("\"action\":\"drop\"", StringComparison.Ordinal)
                )
                .ToArray();
    }

    [Theory]
    [InlineData(1, 5, true)]
    [InlineData(1, 4, false)]
    [InlineData(0, 5, false)]
    public async Task Follow_requires_an_exact_fence_and_keeps_the_original_sender(
        byte hop,
        ulong objectGeneration,
        bool accepted
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var target = new ZLinkManagedMeshNode(context, "follow-wire");
        await using var monitor = target.OpenMonitor();
        var targetRid = RoutingId.From("follow-target");
        target.SetRoutingId(targetRid);
        target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        var endpoint = $"inproc://follow-{Guid.NewGuid():N}";
        target.SetBind(endpoint);
        target.Start();
        target.SetLocalOwnerLeaseGeneration(7);
        using var spot = target.GetOrCreateReservedSpot("instance", 5, 13, out _);

        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        using var forwarder = context.CreateDealerSocket();
        using var completion = new TestCompletionPollerDriver(forwarder);
        await AdmitForwarderAsync(forwarder, endpoint, "follow-wire", deadline.Token);

        var original = RoutingId.From("original-source");
        var snapshot = new ZLinkSpotHandleSnapshot(
            "follow-wire",
            targetRid,
            "instance",
            objectGeneration,
            ZLinkSpotKind.Instance,
            13,
            target.Status().LifecycleGeneration,
            7
        );
        var record = CreateFollow(
            original,
            19,
            snapshot,
            hop == 0 ? (byte)1 : hop,
            hasMetadata: true
        );
        var codecContext = ServiceWireCodec.DecodeContext.Empty;
        var header = ServiceWireCodec.EncodeInstanceSpotFollow55Header(record, codecContext);
        if (hop == 0)
        {
            Assert.Throws<InvalidDataException>(() =>
                ServiceWireCodec.EncodeInstanceSpotFollow55Header(
                    record with
                    {
                        MessageFollowHopCount = new(0),
                    },
                    codecContext
                )
            );
            var encodedSpot = ServiceWireCodec.EncodeSpotRouteFence(
                record.TargetSpot,
                codecContext
            );
            header[header.Length - encodedSpot.Length - 1] = 0;
            Assert.Throws<InvalidDataException>(() =>
                ServiceWireCodec.DecodeInstanceSpotFollow55Header(header, codecContext)
            );
        }

        var flowPath = Path.Combine(Path.GetTempPath(), $"instance-follow-{Guid.NewGuid():N}.flow");
        using var listener = new TestHostMessageFlowListener(flowPath);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        target.SetLogicalMulticastDispatchErrors(
            new ZLinkDispatchErrorReporter(options),
            "follow-wire"
        );
        var protocolErrors = monitor.Status().ProtocolErrors;
        using var body = Message.From(new byte[] { 42 });
        using var wireHeader = Message.From(header);
        var metadata = ServiceWireCodec.EncodeMetadataFrame(
            new ServiceWireCodec.MetadataFrame([new(new("test"), new("follow"))]),
            codecContext
        );
        using var wireMetadata = Message.From(metadata);
        using var payload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart([body])
        );
        await forwarder
            .Send()
            .Messages([wireHeader, wireMetadata, payload])
            .Async(deadline.Token)
            .Admitted;

        await WaitUntilAsync(
            () =>
                target.Status().PendingApplicationMessages != 0
                || monitor.Status().ProtocolErrors != protocolErrors
                || listener.ReadLines().Length != 0,
            deadline.Token
        );
        if (hop == 0)
        {
            Assert.Equal(protocolErrors + 1, monitor.Status().ProtocolErrors);
            Assert.Equal(0UL, target.Status().PendingApplicationMessages);
            return;
        }

        Assert.Equal(protocolErrors, monitor.Status().ProtocolErrors);
        if (!accepted)
        {
            MessageFlowDeliveryTests.WaitForDelivery();
            var diagnostic = Assert.Single(listener.ReadLines());
            Assert.Contains("event=zlink.dispatch_error", diagnostic);
            Assert.Contains("\"action\":\"drop\"", diagnostic);
            Assert.Equal(0UL, target.Status().PendingApplicationMessages);
            return;
        }

        using var ready = new MeshReadyBatch();
        target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(1, ready.Count);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(1, received.Count);
        var message = received[0];
        Assert.Equal(MeshRecordKind.SpotSend, message.Kind);
        Assert.Equal(original, message.SourceNodeRid);
        Assert.Equal(19UL, message.SourceBindingGeneration);
        Assert.Equal("source-spot", message.SourceSpotId);
        Assert.Equal(default, message.OperationId);
        Assert.Equal(0UL, message.ReplyRouteId);
        Assert.Equal(metadata, message.ApplicationMetadata);
        var parts = received.RetainMessage(0);
        try
        {
            Assert.Equal(new byte[] { 42 }, Assert.Single(parts).ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
        Assert.False(claim.Receive(received, RecvFlags.DontWait));
    }

    private static ServiceWireCodec.InstanceSpotFollow55 CreateFollow(
        RoutingId original,
        ulong sourceGeneration,
        ZLinkSpotHandleSnapshot target,
        byte hop,
        bool hasMetadata
    ) =>
        new(
            hasMetadata ? (byte)1 : (byte)0,
            default,
            default,
            new(sourceGeneration),
            new(original),
            new("source-spot"),
            new(hop),
            new(
                new(new(target.SpotId), new(target.Generation)),
                new(target.NodeRid),
                new(target.NodeGeneration),
                new(target.AuthorityOwnerGeneration),
                new(target.OwnerLeaseGeneration)
            ),
            default,
            default
        );

    private static async Task AdmitForwarderAsync(
        IDealerSocket forwarder,
        string endpoint,
        string mesh,
        CancellationToken cancellationToken
    )
    {
        var id = Guid.NewGuid().ToString("N");
        forwarder.SetRoutingId(RoutingId.From($"forwarder-{id}"));
        forwarder.Connect(endpoint);
        var advertised = $"inproc://follow-forwarder-{id}";
        using var hello = Message.From(
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                mesh,
                advertised,
                lifecycleGeneration: 11,
                descriptorRevision: 1,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            )
        );
        await forwarder.Send().Message(hello).Async(cancellationToken).Admitted;
        using var admission = await RawMeshPeerAdmission.CompleteAsync(
            forwarder,
            mesh,
            advertised,
            lifecycleGeneration: 11
        );
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        CancellationToken cancellationToken
    )
    {
        while (!condition())
            await Task.Delay(10, cancellationToken);
    }
}
