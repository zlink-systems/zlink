using System.Collections.Concurrent;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

/// <summary>
/// Runs every scenario of the cross-language Spot Close fixture
/// (framework/runtime/conformance/spot-close-v1.json) against a started .NET
/// runtime with an in-memory Location Store.
/// </summary>
public sealed class SpotCloseConformanceTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Accepted_Missing_intent_reincarnates_on_the_same_owner_or_deletes_failed_initialization(
        bool failInitialize
    )
    {
        await RunCloseBranchAsync(
            failInitialize ? "reincarnate-initialization-fails" : "reincarnate-pending-intent"
        );
    }

    [Fact]
    public async Task Retained_intents_precede_native_request_held_before_instance_publication()
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"native-fifo-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-fifo-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        var close = host.State.ContextCloseTask!;
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        var activation = await host
            .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
            .Catalog.TryGetInstanceActivationAsync(
                spotId,
                SpotCloseHost.InstanceType,
                authority.ObjectGeneration
            );
        Assert.NotNull(activation);
        var first = QueueRetainedIntent(host, activation, authority, spotId, "pending-first");
        var second = QueueRetainedIntent(host, activation, authority, spotId, "pending-second");
        var initialized = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        host.State.OnReinitialize = async generation =>
        {
            Assert.NotEqual(initial.Generation, generation);
            var ready = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await host
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
                )
                .Snapshot;
            Assert.Equal(generation, ready.ObjectGeneration);
            initialized.TrySetResult();
            await release.Task;
        };
        host.State.ReleaseOnClosing.TrySetResult();
        try
        {
            await initialized.Task.WaitAsync(Wait);
            host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>()
                .InvalidateSpotRoute(new ZLinkSpotLocationKey(spotId));
            var before = GetIngressPermits();
            var arrivedDuringInitialization = host.RequestInstanceAsync(spotId, "after-pending");
            await ObserveMappedIngressAsync().WaitAsync(Wait);
            Assert.False(arrivedDuringInitialization.IsCompleted);
            Assert.Empty(host.State.PendingHandlerGenerations);
            release.TrySetResult();
            Assert.Equal(RequestResult.Ok, (await first.WaitAsync(Wait)).Result);
            Assert.Equal(RequestResult.Ok, (await second.WaitAsync(Wait)).Result);
            Assert.Equal(
                "after-pending",
                (await arrivedDuringInitialization.WaitAsync(Wait)).Marker
            );
            Assert.True(await close.WaitAsync(Wait));
            Assert.Equal(
                new[] { "pending-first", "pending-second", "after-pending" },
                host.State.InstanceHandlerMarkers.Where(static marker => marker != "initial")
            );

            ulong GetIngressPermits() =>
                (
                    host.Runtime.GetHostCapacityStatus()
                    ?? throw new InvalidOperationException(
                        "The running test host has no capacity projection."
                    )
                )
                    .ApplicationJobQueue
                    .PermitsInUse;

            async Task ObserveMappedIngressAsync()
            {
                using var ingressDeadline = new CancellationTokenSource(Wait);
                while (GetIngressPermits() <= before)
                {
                    ingressDeadline.Token.ThrowIfCancellationRequested();
                    if (arrivedDuringInitialization.IsCompleted)
                    {
                        await arrivedDuringInitialization;
                        throw new InvalidOperationException(
                            "The native request completed before Instance publication."
                        );
                    }
                    await Task.Yield();
                }
            }
        }
        finally
        {
            release.TrySetResult();
        }
    }

    [Fact]
    public async Task A_second_Close_preserves_the_retained_intent_on_the_next_incarnation()
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"second-close-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-twice-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        var firstClose = host.State.ContextCloseTask!;
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        var activation = await host
            .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
            .Catalog.TryGetInstanceActivationAsync(
                spotId,
                SpotCloseHost.InstanceType,
                authority.ObjectGeneration
            );
        Assert.NotNull(activation);
        var first = QueueRetainedIntent(host, activation, authority, spotId, "pending-close");
        var second = QueueRetainedIntent(host, activation, authority, spotId, "pending-second");
        host.State.ReleaseOnClosing.TrySetResult();
        Assert.Equal(RequestResult.Ok, (await first.WaitAsync(Wait)).Result);
        Assert.Equal(RequestResult.Ok, (await second.WaitAsync(Wait)).Result);
        Assert.True(await firstClose.WaitAsync(Wait));
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        Assert.Equal(
            new[] { "pending-close", "pending-second" },
            host.State.InstanceHandlerMarkers.Where(static marker => marker != "initial")
        );
        var executed = host.State.PendingHandlerGenerations.ToArray();
        Assert.Equal(2, executed.Length);
        Assert.NotEqual(initial.Generation, executed[0]);
        Assert.NotEqual(executed[0], executed[1]);
        Assert.Equal(3, host.State.InitializedGenerations.Count);
    }

    [Fact]
    public async Task Disposing_a_successor_completes_its_unstarted_retained_intent_with_Unavailable()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-successor-stop-{Guid.NewGuid():N}";
        await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        var closed = host.State.ContextCloseTask!;
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;
        var store = host.Runtime.Registration.Locations.ResolveStore()!;
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            )
            .Snapshot;
        var catalog = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Catalog;
        var activation = await catalog.TryGetInstanceActivationAsync(
            spotId,
            SpotCloseHost.InstanceType,
            authority.ObjectGeneration
        );
        Assert.NotNull(activation);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        host.State.OnPendingHandler = async _ =>
        {
            entered.TrySetResult();
            await release.Task;
        };
        try
        {
            var first = QueueRetainedIntent(host, activation, authority, spotId, "pending-hold");
            var second = QueueRetainedIntent(host, activation, authority, spotId, "pending-second");
            host.State.ReleaseOnClosing.TrySetResult();
            await entered.Task.WaitAsync(Wait);
            var successorAuthority = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                    )
                )
                .Snapshot;
            var successor = await catalog.TryGetInstanceActivationAsync(
                spotId,
                SpotCloseHost.InstanceType,
                successorAuthority.ObjectGeneration
            );
            Assert.NotNull(successor);
            Assert.NotSame(activation, successor);
            var disposing = successor.DisposeAsync().AsTask();
            release.TrySetResult();
            await disposing.WaitAsync(Wait);
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                second.WaitAsync(Wait)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            Assert.Equal(RequestResult.Ok, (await first.WaitAsync(Wait)).Result);
            Assert.True(await closed.WaitAsync(Wait));
            Assert.DoesNotContain("pending-second", host.State.InstanceHandlerMarkers);
        }
        finally
        {
            release.TrySetResult();
        }
    }

    private static Task<InstanceSpotActivationTerminal> QueueRetainedIntent(
        SpotCloseHost host,
        ZLinkSpotActivation activation,
        ZLinkAuthoritySnapshot authority,
        string spotId,
        string marker
    )
    {
        var node = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node;
        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                SpotCloseHost.MeshName,
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration,
                spotId,
                SpotCloseHost.InstanceType,
                authority.StoreVersion
            ),
            node.RoutingId,
            node.MeshStatus().LifecycleGeneration,
            string.Empty,
            node.AllocateOperationId(),
            true,
            0,
            checked((ulong)DateTimeOffset.UtcNow.Add(Wait).ToUnixTimeMilliseconds())
        );
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
            ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Request,
                SpotCloseHost.MeshName,
                nameof(SpotCloseProbeRequest),
                Wait
            ),
            new SpotCloseProbeRequest(marker),
            host.Runtime.Registration.Codecs
        );
        try
        {
            return activation
                .DispatchDurableActivationAsync(
                    operation.OperationId,
                    operation.SourceNodeRid,
                    operation.SourceSpotId,
                    new ZLinkServiceWireCodec.RequestSourceFence(
                        authority.OwnerId,
                        checked((ulong)authority.OwnerLeaseGeneration),
                        operation.SourceNodeRid,
                        operation.SourceNodeGeneration
                    ),
                    operation.Target.TargetNodeGeneration,
                    authority.AuthorityOwnerGeneration,
                    checked((ulong)authority.OwnerLeaseGeneration),
                    parts.Select(static part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray(),
                    null,
                    true,
                    CancellationToken.None,
                    operation
                )
                .AsTask();
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static async Task<CloseBranchObservation> RunCloseBranchAsync(
        string name,
        string hostMode = "Serving"
    )
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"branch-{name}-{hostMode}-{Guid.NewGuid():N}.flow"
        );
        using var flow = new TestHostMessageFlowListener(flowPath);
        System.Console.WriteLine($"Message flow file: {flowPath}");
        var failInitialize = name == "reincarnate-initialization-fails";
        var hasIntent =
            name
            is "reincarnate-pending-intent"
                or "reincarnate-initialization-fails"
                or "release-during-host-drain-or-relocation";
        var noIntentOnly = name == "closing-message-without-intent";
        var order = new ConcurrentQueue<string>();
        var objectGenerationChanged = false;
        var ownerGenerationChanged = false;
        var ownerPreserved = false;
        var leasePreserved = false;
        var capacityPreserved = false;
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-instance-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        var oldAuthority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        var closeReply = await host.RequestInstanceAsync(spotId);
        Assert.Equal(initial.Generation, closeReply.Generation);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;
        host.State.ThrowOnInitialize = failInitialize;
        host.State.OnReinitialize = async generation =>
        {
            var observed = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await host
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
                )
                .Snapshot;
            Assert.Equal(generation, observed.ObjectGeneration);
            objectGenerationChanged = oldAuthority.ObjectGeneration != observed.ObjectGeneration;
            ownerGenerationChanged =
                oldAuthority.AuthorityOwnerGeneration != observed.AuthorityOwnerGeneration;
            ownerPreserved = oldAuthority.OwnerId == observed.OwnerId;
            leasePreserved = oldAuthority.OwnerLeaseGeneration == observed.OwnerLeaseGeneration;
            capacityPreserved = oldAuthority.Allocation == observed.Allocation;
            Assert.True(objectGenerationChanged);
            order.Enqueue("authorityReincarnated");
            if (!failInitialize)
                order.Enqueue("newIncarnationInitialized");
        };
        host.State.BranchOrder = order;
        var current = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        var activation = await host
            .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
            .Catalog.TryGetInstanceActivationAsync(
                spotId,
                SpotCloseHost.InstanceType,
                current.ObjectGeneration
            );
        Assert.NotNull(activation);
        var pending = hasIntent
            ? QueueRetainedIntent(host, activation, current, spotId, "pending")
            : null;
        Assert.Equal(hasIntent, activation.HasPendingCreationIntent);
        var noIntent = await host.RequestAsync(spotId);
        Assert.Equal(
            ZLinkFrameworkErrorKind.NotFound,
            Assert.IsType<ZLinkFrameworkException>(noIntent).Kind
        );
        var callsBefore = host.State.HandlerCalls;
        var initializedBefore = host.State.InitializedGenerations.Count;
        if (hostMode != "Serving")
            host.Runtime.DrainAdmission.BeginDrain(
                hostMode == "Draining" ? ZLinkDrainOwner.Shutdown : ZLinkDrainOwner.Relocation
            );
        string? terminalKind = noIntentOnly ? "NotFound" : null;
        var terminalCount = noIntentOnly ? 1 : 0;
        if (noIntentOnly)
        {
            Assert.Equal(callsBefore, host.State.HandlerCalls);
            Assert.Equal(initializedBefore, host.State.InitializedGenerations.Count);
        }
        host.State.ReleaseOnClosing.TrySetResult();
        if (failInitialize)
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                pending!.WaitAsync(Wait)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InternalFailure, failure.Kind);
            await Assert.ThrowsAsync<SpotCloseProbeFailure>(() =>
                host.State.ContextCloseTask!.WaitAsync(Wait)
            );
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            order.Enqueue("newGenerationDeleted");
            order.Enqueue("pendingMessagesTypedFailure");
            Assert.Equal(callsBefore, host.State.HandlerCalls);
            terminalKind = "typedFailure";
            terminalCount = 1;
        }
        else if (hostMode != "Serving")
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                pending!.WaitAsync(Wait)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            order.Enqueue("authorityReleased");
            order.Enqueue("missingPlacement");
            terminalCount = 1;
        }
        else if (hasIntent)
        {
            var terminal = await pending!.WaitAsync(Wait);
            Assert.Equal(RequestResult.Ok, terminal.Result);
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            var after = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await host
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
                )
                .Snapshot;
            Assert.NotEqual(oldAuthority.ObjectGeneration, after.ObjectGeneration);
            Assert.NotEqual(oldAuthority.AuthorityOwnerGeneration, after.AuthorityOwnerGeneration);
            Assert.Equal(oldAuthority.OwnerId, after.OwnerId);
            Assert.Equal(oldAuthority.OwnerLeaseGeneration, after.OwnerLeaseGeneration);
            Assert.Equal(oldAuthority.Allocation, after.Allocation);
            Assert.Equal(callsBefore + 1, host.State.HandlerCalls);
            Assert.Equal(after.ObjectGeneration, host.State.InitializedGenerations.Last());
            terminalKind = "reply";
            terminalCount = 1;
        }
        else
        {
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            if (!noIntentOnly)
                order.Enqueue("authorityReleased");
        }
        if (hostMode == "Serving" && hasIntent)
        {
            Assert.Equal(2, host.State.InitializedGenerations.Count);
            Assert.NotEqual(initial.Generation, host.State.InitializedGenerations.Last());
        }
        return new CloseBranchObservation(
            await host.AuthorityAsync(spotId),
            order.ToArray(),
            host.State.PendingHandlerGenerations.Count(generation =>
                generation == initial.Generation
            ),
            host.State.PendingHandlerGenerations.Count(generation =>
                generation != initial.Generation
            ),
            host.State.InitializedGenerations.Count - initializedBefore,
            terminalKind,
            terminalCount,
            objectGenerationChanged,
            ownerGenerationChanged,
            ownerPreserved,
            leasePreserved,
            capacityPreserved
        );
    }

    private sealed record CloseBranchObservation(
        string Authority,
        string[] Order,
        int OldHandlerCalls,
        int NewHandlerCalls,
        int FactoryCalls,
        string? Terminal,
        int TerminalCount,
        bool ObjectGenerationChanged,
        bool OwnerGenerationChanged,
        bool OwnerPreserved,
        bool LeasePreserved,
        bool CapacityPreserved
    );

    [Fact]
    public async Task Spot_close_fixture_scenarios_hold_on_the_dotnet_runtime()
    {
        using var fixture = JsonDocument.Parse(
            await File.ReadAllTextAsync(
                Path.Combine(
                    Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework",
                    "runtime",
                    "conformance",
                    "spot-close-v1.json"
                )
            )
        );
        var root = fixture.RootElement;
        Assert.Equal("zlink.framework.spot-close", root.GetProperty("fixture").GetString());
        Assert.Equal(1, root.GetProperty("version").GetInt32());

        // Every scenario runs; the failures of all scenarios are reported together.
        var failures = new List<string>();
        foreach (var scenario in root.GetProperty("scenarios").EnumerateArray())
        {
            var name = scenario.GetProperty("name").GetString()!;
            try
            {
                var observed = await RunScenarioAsync(name, scenario.GetProperty("given"))
                    .WaitAsync(TimeSpan.FromSeconds(60));
                AssertExpectation(name, scenario.GetProperty("expect"), observed);
            }
            catch (Exception exception)
            {
                failures.Add(
                    $"{name}: {exception.GetType().Name}: {exception.Message.ReplaceLineEndings(" ")}"
                );
            }
        }
        foreach (var branch in root.GetProperty("closeBranches").EnumerateArray())
        {
            var name = branch.GetProperty("name").GetString()!;
            try
            {
                var given = branch.GetProperty("given");
                var modes =
                    given.TryGetProperty("host", out var hosts)
                    && hosts.ValueKind == JsonValueKind.Array
                        ? hosts.EnumerateArray().Select(static item => item.GetString()!).ToArray()
                        : ["Serving"];
                foreach (var mode in modes)
                {
                    var observed = await RunCloseBranchAsync(name, mode)
                        .WaitAsync(TimeSpan.FromSeconds(60));
                    AssertCloseBranch(branch.GetProperty("expect"), observed);
                }
            }
            catch (Exception exception)
            {
                failures.Add(
                    $"{name}: {exception.GetType().Name}: {exception.Message.ReplaceLineEndings(" ")}"
                );
            }
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    [Fact]
    public async Task Context_close_completes_after_authority_release()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spot = await host.CreateSpotAsync();
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";

        Assert.Equal("handlerReply", await host.RequestAsync(spot.SpotId));
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        var close = host.State.ContextCloseTask;
        Assert.NotNull(close);
        Assert.False(close.IsCompleted);
        Assert.Equal("Closing", await host.AuthorityAsync(spot.SpotId));

        host.State.ReleaseOnClosing.TrySetResult();
        Assert.True(await close.WaitAsync(Wait));
        Assert.Equal("Missing", await host.AuthorityAsync(spot.SpotId));
    }

    [Fact]
    public async Task Ready_instance_intent_waits_for_successor_while_owner_is_closing()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-ready-intent-{Guid.NewGuid():N}";
        var first = await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;

        var withoutIntent = Assert.IsType<ZLinkFrameworkException>(await host.RequestAsync(spotId));
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, withoutIntent.Kind);

        var pending = host.RequestInstanceAsync(spotId, "ready-intent");
        try
        {
            Assert.False(pending.IsCompleted);
            var authority = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(
                    await host
                        .Runtime.Registration.Locations.ResolveStore()!
                        .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
                )
                .Snapshot;
            var closing = await host
                .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
                .Catalog.TryGetInstanceActivationAsync(
                    spotId,
                    SpotCloseHost.InstanceType,
                    authority.ObjectGeneration
                );
            Assert.NotNull(closing);
            await ObserveOwnerAcceptanceAsync(closing).WaitAsync(Wait);
            host.State.ReleaseOnClosing.TrySetResult();
            var reply = await pending.WaitAsync(Wait);
            Assert.NotEqual(first.Generation, reply.Generation);
            Assert.Equal("ready-intent", reply.Marker);
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
        }
    }

    [Fact]
    public async Task Ready_instance_send_keeps_its_intent_across_close()
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"ready-send-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-ready-send-{Guid.NewGuid():N}";
        var first = await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;

        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        var closing = await host
            .Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName)
            .Catalog.TryGetInstanceActivationAsync(
                spotId,
                SpotCloseHost.InstanceType,
                authority.ObjectGeneration
            );
        Assert.NotNull(closing);

        try
        {
            await host.SendInstanceAsync(spotId, "ready-send");
            await ObserveOwnerAcceptanceAsync(closing).WaitAsync(Wait);
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            host.Runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>()
                .InvalidateSpotRoute(new ZLinkSpotLocationKey(spotId));
            var after = await host.RequestInstanceAsync(spotId, "after-ready-send").WaitAsync(Wait);
            Assert.NotEqual(first.Generation, after.Generation);
            Assert.Contains("ready-send", host.State.InstanceHandlerMarkers);
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
        }
    }

    private static async Task ObserveOwnerAcceptanceAsync(ZLinkSpotActivation closing)
    {
        while (!closing.HasPendingCreationIntent)
            await Task.Yield();
    }

    private static async Task<SpotCloseObservation> RunScenarioAsync(string name, JsonElement given)
    {
        await using (var host = await SpotCloseHost.StartAsync())
        {
            return name switch
            {
                "close-absent-incarnation-is-false" => await CloseAbsentAsync(host),
                "close-other-generation-is-invalid-operation" => await CloseOtherGenerationAsync(
                    host
                ),
                "close-with-membership-is-false-and-keeps-authority" =>
                    await CloseWithMembershipAsync(host),
                "close-after-accepted-join-observes-its-membership" =>
                    await CloseAfterAcceptedJoinAsync(host),
                "failure-before-closing-commit-keeps-authority" =>
                    await FailureBeforeClosingCommitAsync(host, given),
                "on-closing-failure-is-diagnostic-and-cleanup-continues" =>
                    await OnClosingFailureAsync(host, given),
                _ => throw new Xunit.Sdk.XunitException(
                    $"Spot Close fixture scenario '{name}' has no .NET runner."
                ),
            };
        }
    }

    private static async Task<SpotCloseObservation> CloseAbsentAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        var absent = new SpotRef(
            $"absent-{Guid.NewGuid():N}",
            spot.ObjectGeneration,
            spot.MeshName,
            spot.NodeRid
        );
        Assert.Equal("Missing", await host.AuthorityAsync(absent.SpotId));
        return new SpotCloseObservation
        {
            Result = await host.CloseAsync(absent),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> CloseOtherGenerationAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        var result = await host.CloseAsync(
            spot with
            {
                ObjectGeneration = spot.ObjectGeneration + 1,
            }
        );
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> CloseWithMembershipAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        await host.JoinActorAsync(spot.SpotId);
        await host.State.Joined.Task.WaitAsync(Wait);
        var result = await host.CloseAsync(spot);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    // The Join is accepted on the Spot lifecycle lane first and held in
    // OnActorJoinAsync; the manager Close submitted meanwhile must observe the
    // membership that Join decides.
    private static async Task<SpotCloseObservation> CloseAfterAcceptedJoinAsync(SpotCloseHost host)
    {
        var spot = await host.CreateSpotAsync();
        host.State.HoldJoin = true;
        await host.JoinActorAsync(spot.SpotId);
        await host.State.JoinEntered.Task.WaitAsync(Wait);
        var close = host.CloseAsync(spot)
            .ContinueWith(
                task =>
                {
                    host.State.Order.Enqueue("closeCompleted");
                    return task.Result;
                },
                TaskScheduler.Default
            );
        host.State.ReleaseJoin.TrySetResult();
        var result = await close.WaitAsync(Wait);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
            Order = host.State.Order.ToArray(),
        };
    }

    private static async Task<SpotCloseObservation> FailureBeforeClosingCommitAsync(
        SpotCloseHost host,
        JsonElement given
    )
    {
        Assert.Equal("ownerFenceMismatch", given.GetProperty("closingCommit").GetString());
        var spot = await host.CreateSpotAsync();
        host.Store.ConflictPutsFor = spot.SpotId;
        var result = await host.CloseAsync(spot);
        host.Store.ConflictPutsFor = null;
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            Admission = await host.AdmissionAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
        };
    }

    private static async Task<SpotCloseObservation> OnClosingFailureAsync(
        SpotCloseHost host,
        JsonElement given
    )
    {
        Assert.Equal("throws", given.GetProperty("onClosing").GetString());
        var spot = await host.CreateSpotAsync();
        host.State.ThrowOnClosing = true;
        var diagnostics = new ConcurrentQueue<string>();
        host.Runtime.ErrorSink.UnhandledCallbackException += exception =>
        {
            for (
                Exception? current = exception;
                current is not null;
                current = current.InnerException
            )
                if (current is SpotCloseProbeFailure)
                {
                    diagnostics.Enqueue("onClosingFailed");
                    return;
                }
        };
        var result = await host.CloseAsync(spot);
        return new SpotCloseObservation
        {
            Result = result,
            Authority = await host.AuthorityAsync(spot.SpotId),
            OnClosingCalls = host.State.OnClosingCalls,
            Diagnostics = diagnostics.ToArray(),
        };
    }

    private static void AssertExpectation(
        string scenario,
        JsonElement expect,
        SpotCloseObservation observed
    )
    {
        foreach (var property in expect.EnumerateObject())
        {
            var expected = property.Value;
            switch (property.Name)
            {
                case "result":
                    AssertValue(scenario, property.Name, expected, observed.Result);
                    break;
                case "results":
                    Assert.NotNull(observed.Results);
                    Assert.Equal(expected.GetArrayLength(), observed.Results!.Length);
                    for (var index = 0; index < observed.Results.Length; index++)
                        AssertValue(
                            scenario,
                            property.Name,
                            expected[index],
                            observed.Results[index]
                        );
                    break;
                case "authority":
                    Assert.Equal(expected.GetString(), observed.Authority);
                    break;
                case "authorityAfterFailure":
                    Assert.Equal(expected.GetString(), observed.AuthorityAfterFailure);
                    break;
                case "admission":
                    Assert.Equal(expected.GetString(), observed.Admission);
                    break;
                case "onClosingCalls":
                    Assert.Equal(expected.GetInt32(), observed.OnClosingCalls);
                    break;
                case "handlerCalls":
                    Assert.Equal(expected.GetInt32(), observed.HandlerCalls);
                    break;
                case "creationIntent":
                    Assert.Equal(expected.GetBoolean(), observed.CreationIntent);
                    break;
                case "order":
                    Assert.Equal(
                        expected.EnumerateArray().Select(static item => item.GetString()!),
                        observed.Order ?? []
                    );
                    break;
                case "diagnostics":
                    foreach (var diagnostic in expected.EnumerateArray())
                        Assert.Contains(diagnostic.GetString()!, observed.Diagnostics ?? []);
                    break;
                default:
                    throw new Xunit.Sdk.XunitException(
                        $"Scenario '{scenario}' expects unknown field '{property.Name}'."
                    );
            }
        }
    }

    private static void AssertCloseBranch(JsonElement expected, CloseBranchObservation observed)
    {
        foreach (var field in expected.EnumerateObject())
        {
            switch (field.Name)
            {
                case "order":
                    Assert.Equal(
                        field.Value.EnumerateArray().Select(static value => value.GetString()!),
                        observed.Order
                    );
                    break;
                case "authority":
                    Assert.Equal(field.Value.GetString(), observed.Authority);
                    break;
                case "oldHandlerCalls":
                    Assert.Equal(field.Value.GetInt32(), observed.OldHandlerCalls);
                    break;
                case "newHandlerCalls":
                    Assert.Equal(field.Value.GetInt32(), observed.NewHandlerCalls);
                    break;
                case "factoryCalls":
                case "thisHostFactoryCalls":
                    Assert.Equal(field.Value.GetInt32(), observed.FactoryCalls);
                    break;
                case "messageTerminal":
                    Assert.Equal(field.Value.GetString(), observed.Terminal);
                    break;
                case "messageTerminalCount":
                    Assert.Equal(field.Value.GetInt32(), observed.TerminalCount);
                    break;
                case "objectGeneration":
                    Assert.Equal("storeIssuedDifferent", field.Value.GetString());
                    Assert.True(observed.ObjectGenerationChanged);
                    break;
                case "authorityOwnerGeneration":
                    Assert.Equal("storeIssuedDifferent", field.Value.GetString());
                    Assert.True(observed.OwnerGenerationChanged);
                    break;
                case "owner":
                    Assert.Equal("unchanged", field.Value.GetString());
                    Assert.True(observed.OwnerPreserved);
                    break;
                case "lease":
                    Assert.Equal("unchanged", field.Value.GetString());
                    Assert.True(observed.LeasePreserved);
                    break;
                case "capacity":
                    Assert.Equal("unchanged", field.Value.GetString());
                    Assert.True(observed.CapacityPreserved);
                    break;
                default:
                    throw new Xunit.Sdk.XunitException(
                        $"Unknown Close branch expectation '{field.Name}'."
                    );
            }
        }
    }

    private static void AssertValue(
        string scenario,
        string field,
        JsonElement expected,
        object? observed
    )
    {
        switch (expected.ValueKind)
        {
            case JsonValueKind.True:
            case JsonValueKind.False:
                Assert.True(
                    observed is bool value && value == expected.GetBoolean(),
                    $"{scenario}.{field}: expected {expected}, observed {observed}"
                );
                break;
            case JsonValueKind.String when expected.GetString() == "failure":
                Assert.True(
                    observed is Exception,
                    $"{scenario}.{field}: expected a failure, observed {observed}"
                );
                break;
            case JsonValueKind.String:
                Assert.Equal(expected.GetString(), Describe(observed));
                break;
            default:
                throw new Xunit.Sdk.XunitException(
                    $"Scenario '{scenario}' has an unsupported {field} value {expected}."
                );
        }
    }

    private static string? Describe(object? observed) =>
        observed switch
        {
            ZLinkFrameworkException framework => framework.Kind.ToString(),
            Exception exception => exception.GetType().Name,
            _ => observed?.ToString(),
        };
}

internal sealed record SpotCloseObservation
{
    public object? Result { get; init; }
    public object?[]? Results { get; init; }
    public string? Authority { get; init; }
    public string? AuthorityAfterFailure { get; init; }
    public string? Admission { get; init; }
    public int OnClosingCalls { get; init; }
    public int HandlerCalls { get; init; }
    public bool CreationIntent { get; init; }
    public string[]? Order { get; init; }
    public string[]? Diagnostics { get; init; }
}

internal sealed class SpotCloseHost : IAsyncDisposable
{
    internal const string MeshName = "spot-close";
    internal const string SpotType = "spot-close-probe";
    internal const string ActorType = "spot-close-actor";
    internal const string InstanceType = "spot-close-instance";

    private readonly ServiceProvider _provider;
    private readonly IHostedService _hosted;

    private SpotCloseHost(
        ServiceProvider provider,
        IHostedService hosted,
        SpotCloseFaultStore store,
        SpotCloseProbeState state
    )
    {
        _provider = provider;
        _hosted = hosted;
        Store = store;
        State = state;
        Runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        Manager = provider.GetRequiredService<IZLinkSpotManager>();
    }

    internal ZLinkFrameworkRuntime Runtime { get; }
    internal IZLinkSpotManager Manager { get; }
    internal SpotCloseFaultStore Store { get; }
    internal SpotCloseProbeState State { get; }

    internal static async Task<SpotCloseHost> StartAsync()
    {
        var store = new SpotCloseFaultStore(new ZLinkInMemoryProviderLocationStore());
        var state = new SpotCloseProbeState();
        var services = new ServiceCollection();
        services.AddSingleton(state);
        services.AddTransient<SpotCloseProbeHandler>();
        services.AddScoped<SpotCloseScopedResource>();
        services.AddTransient<SpotCloseJoinHandler>();
        services.AddTransient<SpotCloseInstanceHandler>();
        services.AddTransient<SpotCloseInstanceSignalHandler>();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            options.ConfigureLocations().PollingInterval = TimeSpan.FromMilliseconds(10);
            options
                .AddRouteMesh(MeshName)
                .Listen("tcp://127.0.0.1:0")
                .SetRoutingIdPrefix("spot-close")
                .SetActorLimit(100)
                .SetSpotLimit(100)
                .Objects()
                .Server()
                .AddEntrySpot<SpotCloseEntrySpot>()
                .AddActorFactory<SpotCloseActor, SpotCloseActorFactory>(
                    ActorType,
                    static factory => factory.DisableRelocation()
                )
                .AddSpotFactory<SpotCloseProbeSpot>(
                    SpotType,
                    static factory => factory.DisableRelocation()
                )
                .AddInstanceSpotFactory<SpotCloseInstance>(
                    InstanceType,
                    static factory => factory.DisableRelocation()
                );
        });
        var provider = services.BuildServiceProvider();
        provider
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(static service => service is ZLinkFrameworkHostedService);
        try
        {
            await hosted.StartAsync(CancellationToken.None);
            return new SpotCloseHost(provider, hosted, store, state);
        }
        catch
        {
            await provider.DisposeAsync();
            throw;
        }
    }

    internal async Task<SpotRef> CreateSpotAsync() =>
        (await Manager.Create(SpotType).InMesh(MeshName).Async()).Spot;

    internal async Task<SpotCloseInstanceReply> RequestInstanceAsync(
        string spotId,
        string marker = "initial"
    ) =>
        await _provider
            .GetRequiredService<IZLinkSpotClient>()
            .RequestToSpot(spotId, new SpotCloseProbeRequest(marker))
            .InstanceSpot(InstanceType)
            .InMesh(MeshName)
            .Timeout(TimeSpan.FromSeconds(10))
            .Async<SpotCloseInstanceReply>();

    internal ValueTask SendInstanceAsync(string spotId, string marker) =>
        _provider
            .GetRequiredService<IZLinkSpotClient>()
            .SendToSpot(spotId, new SpotCloseProbeSignal(marker))
            .InstanceSpot(InstanceType)
            .InMesh(MeshName)
            .Async();

    internal async Task<object?> CloseAsync(SpotRef spot)
    {
        try
        {
            return await Manager.CloseAsync(spot);
        }
        catch (Exception exception)
        {
            return exception;
        }
    }

    // Instance-intent-less direct request to the Spot by its ID.
    internal async Task<object?> RequestAsync(string spotId)
    {
        try
        {
            _ = await _provider
                .GetRequiredService<IZLinkSpotClient>()
                .RequestToSpot(spotId, new SpotCloseProbeRequest("probe"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<SpotCloseProbeReply>();
            return "handlerReply";
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Origin == ZLinkErrorOrigin.Application)
        {
            return "handlerFailure";
        }
        catch (Exception exception)
        {
            return exception;
        }
    }

    internal async Task<string> AdmissionAsync(string spotId) =>
        await RequestAsync(spotId) is "handlerReply" ? "open" : "sealed";

    internal async Task JoinActorAsync(string spotId)
    {
        var actorId = $"spot-close-actor-{Guid.NewGuid():N}";
        _ = Assert.IsType<ZLinkActorCreateResult.Created>(
            await _provider
                .GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, ActorType)
                .InMesh(MeshName)
                .Request(ZLinkMessage.Empty)
                .Timeout(TimeSpan.FromSeconds(10))
                .Async()
        );
        _ = await _provider
            .GetRequiredService<IZLinkActorClient>()
            .RequestToActor(actorId, new SpotCloseJoin(spotId))
            .Timeout(TimeSpan.FromSeconds(10))
            .Async<SpotCloseProbeReply>();
    }

    internal async Task<string> AuthorityAsync(string spotId)
    {
        var store = Runtime.Registration.Locations.ResolveStore()!;
        var read = await store.ReadAuthorityAsync(
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId),
            CancellationToken.None
        );
        if (read is not ZLinkAuthorityReadResult.Found found)
            return "Missing";
        if (ZLinkUserSpotAuthorityPayloadCodec.TryDecode(found.Snapshot.Payload.Span, out var user))
            return user.State.ToString();
        Assert.True(
            ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var instance
            )
        );
        return instance.State.ToString();
    }

    public async ValueTask DisposeAsync()
    {
        State.ReleaseOnClosing.TrySetResult();
        State.ReleaseJoin.TrySetResult();
        if (Runtime.IsStarted)
            await _hosted.StopAsync(CancellationToken.None);
        await _provider.DisposeAsync();
    }
}

internal sealed class SpotCloseProbeState
{
    private int _handlerCalls;
    private int _onClosingCalls;

    internal int HandlerCalls => Volatile.Read(ref _handlerCalls);
    internal int OnClosingCalls => Volatile.Read(ref _onClosingCalls);
    internal ConcurrentQueue<string> Order { get; } = new();
    internal string? HandlerMode { get; set; }
    internal Func<CancellationToken, Task>? OnPendingHandler { get; set; }
    internal Task<bool>? ContextCloseTask { get; set; }
    internal bool ThrowOnClosing { get; set; }
    internal bool HoldOnClosing { get; set; }
    internal bool HoldJoin { get; set; }
    internal bool ThrowOnInitialize { get; set; }
    internal ConcurrentQueue<ulong> InitializedGenerations { get; } = new();
    internal Func<ulong, Task>? OnReinitialize { get; set; }
    internal ConcurrentQueue<string>? BranchOrder { get; set; }
    internal string? StoredMarker { get; set; }
    internal ConcurrentQueue<ulong> PendingHandlerGenerations { get; } = new();
    internal ConcurrentQueue<string> InstanceHandlerMarkers { get; } = new();

    internal TaskCompletionSource OnClosingEntered { get; } = Signal();
    internal TaskCompletionSource ReleaseOnClosing { get; } = Signal();
    internal TaskCompletionSource JoinEntered { get; } = Signal();
    internal TaskCompletionSource ReleaseJoin { get; } = Signal();
    internal TaskCompletionSource Joined { get; } = Signal();

    internal void RecordHandlerCall() => Interlocked.Increment(ref _handlerCalls);

    internal void RecordOnClosing() => Interlocked.Increment(ref _onClosingCalls);

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}

internal sealed class SpotCloseProbeFailure() : Exception("OnClosing probe failure.");

internal sealed record SpotCloseProbeRequest(string Marker);

internal sealed record SpotCloseProbeSignal(string Marker);

internal sealed record SpotCloseProbeReply(string Marker);

internal sealed record SpotCloseInstanceReply(string Marker, ulong Generation);

internal sealed class SpotCloseInstance(
    IZLinkInstanceSpotContext context,
    SpotCloseProbeState state
) : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;
    internal SpotCloseProbeState State { get; } = state;

    internal string? RestoredMarker { get; private set; }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        State.InitializedGenerations.Enqueue(Context.ObjectGeneration);
        if (State.OnReinitialize is { } observe)
            await observe(Context.ObjectGeneration);
        if (State.ThrowOnInitialize)
            throw new SpotCloseProbeFailure();
        RestoredMarker = State.StoredMarker;
        if (State.OnReinitialize is not null)
        {
            Assert.Equal("initial", RestoredMarker);
            State.BranchOrder?.Enqueue("storedStateRestored");
        }
    }

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken
    )
    {
        State.RecordOnClosing();
        State.OnClosingEntered.TrySetResult();
        if (State.HoldOnClosing)
            await State.ReleaseOnClosing.Task.WaitAsync(cleanupCancellationToken);
    }
}

internal sealed class SpotCloseInstanceHandler
    : IZLinkSpotRequestHandler<SpotCloseInstance, SpotCloseProbeRequest, SpotCloseInstanceReply>
{
    public async ValueTask<SpotCloseInstanceReply> HandleAsync(
        SpotCloseInstance spot,
        SpotCloseProbeRequest request,
        CancellationToken cancellationToken
    )
    {
        spot.State.RecordHandlerCall();
        spot.State.InstanceHandlerMarkers.Enqueue(request.Marker);
        if (request.Marker == "initial")
            spot.State.StoredMarker = request.Marker;
        if (request.Marker.StartsWith("pending", StringComparison.Ordinal))
        {
            spot.State.PendingHandlerGenerations.Enqueue(spot.Context.ObjectGeneration);
            Assert.Equal("initial", spot.RestoredMarker);
            spot.State.BranchOrder?.Enqueue("pendingIntentMessagesExecuted");
        }
        if (request.Marker == "pending-hold" && spot.State.OnPendingHandler is { } pendingHandler)
            await pendingHandler(cancellationToken);
        if (spot.State.HandlerMode == "closeAndReturn" || request.Marker == "pending-close")
            spot.State.ContextCloseTask = spot.Context.CloseAsync().AsTask();
        return new SpotCloseInstanceReply(request.Marker, spot.Context.ObjectGeneration);
    }
}

internal sealed class SpotCloseInstanceSignalHandler
    : IZLinkSpotPacketHandler<SpotCloseInstance, SpotCloseProbeSignal>
{
    public ValueTask HandleAsync(
        SpotCloseInstance spot,
        SpotCloseProbeSignal message,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.State.InstanceHandlerMarkers.Enqueue(message.Marker);
        return ValueTask.CompletedTask;
    }
}

internal sealed record SpotCloseJoin(string SpotId);

internal sealed class SpotCloseActor(IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = context.ActorId;
    public IZLinkActorContext Context { get; } = context;
}

internal sealed class SpotCloseActorFactory : IZLinkActorFactory<SpotCloseActor>
{
    public ValueTask<SpotCloseActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default
    ) => ValueTask.FromResult(new SpotCloseActor(context));
}

internal sealed class SpotCloseEntrySpot(IZLinkEntrySpotContext context)
    : IZLinkEntrySpot<SpotCloseActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<SpotCloseJoinHandler, SpotCloseActor>(
            nameof(SpotCloseJoin)
        );

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        SpotCloseActor actor,
        ZLinkMessage request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public ValueTask OnJoinedActorAsync(
        SpotCloseActor actor,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(SpotCloseActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class SpotCloseJoinHandler
    : IZLinkEntrySpotActorRequestHandler<
        SpotCloseEntrySpot,
        SpotCloseActor,
        SpotCloseJoin,
        SpotCloseProbeReply
    >
{
    public ValueTask<SpotCloseProbeReply> HandleAsync(
        SpotCloseEntrySpot spot,
        SpotCloseActor actor,
        IZLinkMessageContext context,
        SpotCloseJoin request,
        CancellationToken cancellationToken
    )
    {
        actor.Context.JoinSpot(request.SpotId).Timeout(TimeSpan.FromSeconds(10)).Defer();
        return ValueTask.FromResult(new SpotCloseProbeReply("join"));
    }
}

internal sealed class SpotCloseProbeSpot(
    IZLinkSpotContext context,
    SpotCloseProbeState state,
    SpotCloseScopedResource resource
) : IZLinkSpot<SpotCloseActor>
{
    public IZLinkSpotContext Context { get; } = context;

    internal SpotCloseProbeState State { get; } = state;

    internal SpotCloseScopedResource Resource { get; } = resource;

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken
    )
    {
        if (State.HoldJoin)
        {
            State.JoinEntered.TrySetResult();
            await State.ReleaseJoin.Task.WaitAsync(cancellationToken);
        }
        return ZLinkSpotActorJoinResult.Accept(request);
    }

    public ValueTask OnJoinedActorAsync(SpotCloseActor actor, CancellationToken cancellationToken)
    {
        State.Order.Enqueue("joinCompleted");
        State.Joined.TrySetResult();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(SpotCloseActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken
    )
    {
        State.RecordOnClosing();
        State.Order.Enqueue("onClosing");
        if (State.HoldOnClosing)
        {
            State.OnClosingEntered.TrySetResult();
            await State.ReleaseOnClosing.Task.WaitAsync(cleanupCancellationToken);
        }
        if (State.ThrowOnClosing)
            throw new SpotCloseProbeFailure();
    }
}

internal sealed class SpotCloseProbeHandler
    : IZLinkSpotRequestHandler<SpotCloseProbeSpot, SpotCloseProbeRequest, SpotCloseProbeReply>
{
    public ValueTask<SpotCloseProbeReply> HandleAsync(
        SpotCloseProbeSpot spot,
        SpotCloseProbeRequest request,
        CancellationToken cancellationToken
    )
    {
        spot.State.RecordHandlerCall();
        switch (spot.State.HandlerMode)
        {
            case "closeAndReturn":
                spot.State.ContextCloseTask = spot.Context.CloseAsync().AsTask();
                break;
        }
        return ValueTask.FromResult(new SpotCloseProbeReply(request.Marker));
    }
}

// Provider-level Location Store that injects a lost Closing CAS.
internal sealed class SpotCloseFaultStore(IZLinkLocationStore inner) : IZLinkLocationStore
{
    internal string? ConflictPutsFor { get; set; }

    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    ) => inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    )
    {
        if (
            ConflictPutsFor is { } conflict
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Put put && put.Key.Value.Contains(conflict)
            )
        )
        {
            var now = await inner.ReadAsync(
                new ZLinkStoreKey("spot-close-clock"),
                cancellationToken
            );
            return new ZLinkStoreWriteResult.Conflict(
                now is ZLinkStoreReadResult.Missing missing
                    ? missing.StoreNow
                    : DateTimeOffset.UtcNow
            );
        }
        return await inner.WriteAsync(request, cancellationToken);
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    ) => inner.ScanAsync(request, cancellationToken);
}

// A scoped resource of the Spot activation scope.
internal sealed class SpotCloseScopedResource : IAsyncDisposable
{
    public ValueTask DisposeAsync() => ValueTask.CompletedTask;
}
