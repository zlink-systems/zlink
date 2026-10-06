using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Diagnostics;
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

    [Fact]
    public async Task Flow_file_snapshots_are_complete_while_dispatch_events_are_written()
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"snapshot-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        using var activities = new ActivitySource("Zlink.Framework");
        const int eventCount = 1000;
        var snapshotId = Guid.NewGuid().ToString("N");
        var writer = Task.Run(() =>
        {
            for (var index = 0; index < eventCount; index++)
            {
                using var activity = activities.StartActivity("zlink.dispatch_error");
                activity!.SetTag("snapshot_test", snapshotId);
                activity.SetTag("sequence", index);
            }
        });
        do
        {
            foreach (var line in listener.ReadLines())
            {
                var tagsOffset = line.IndexOf(" tags=", StringComparison.Ordinal);
                using var tags = JsonDocument.Parse(line[(tagsOffset + " tags=".Length)..]);
                if (
                    tags.RootElement.TryGetProperty("snapshot_test", out var id)
                    && id.GetString() == snapshotId
                )
                    Assert.InRange(
                        tags.RootElement.GetProperty("sequence").GetInt32(),
                        0,
                        eventCount - 1
                    );
            }
            await Task.Yield();
        } while (!writer.IsCompleted);
        await writer;
        Assert.Equal(
            eventCount,
            listener
                .ReadLines()
                .Count(line =>
                    line.Contains($"\"snapshot_test\":\"{snapshotId}\"", StringComparison.Ordinal)
                )
        );
    }

    [Theory]
    [InlineData(
        ZLinkFrameworkErrorKind.NotFound,
        RequestResult.NotFound,
        (int)ServiceWireConstants.FrameworkErrorCode.RequestTargetNotFound
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.AlreadyExists,
        RequestResult.Conflict,
        (int)ServiceWireConstants.FrameworkErrorCode.ActorAlreadyExists
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.TypeMismatch,
        RequestResult.Conflict,
        (int)ServiceWireConstants.FrameworkErrorCode.SpotTypeMismatch
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.NotConfigured,
        RequestResult.InternalError,
        (int)ServiceWireConstants.FrameworkErrorCode.RequestFailed
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.Rejected,
        RequestResult.Rejected,
        (int)ServiceWireConstants.FrameworkErrorCode.RequestRejected
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.Unavailable,
        RequestResult.InternalError,
        (int)ServiceWireConstants.FrameworkErrorCode.RouteNotConnected
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.DeadlineExceeded,
        RequestResult.InternalError,
        (int)ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.ShuttingDown,
        RequestResult.Terminated,
        (int)ServiceWireConstants.FrameworkErrorCode.None
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.ProtocolError,
        RequestResult.ProtocolError,
        (int)ServiceWireConstants.FrameworkErrorCode.RequestProtocolError
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.InvalidOperation,
        RequestResult.InvalidState,
        (int)ServiceWireConstants.FrameworkErrorCode.None
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.DataLost,
        RequestResult.InternalError,
        (int)ServiceWireConstants.FrameworkErrorCode.RelocationDataLost
    )]
    [InlineData(
        ZLinkFrameworkErrorKind.InternalFailure,
        RequestResult.InternalError,
        (int)ServiceWireConstants.FrameworkErrorCode.RequestFailed
    )]
    public void Instance_factory_failure_reply_uses_canonical_terminal_pair(
        ZLinkFrameworkErrorKind kind,
        RequestResult result,
        int failureCode
    )
    {
        var actual = ZLinkRequestFailureMapper.TargetFailureReply(
            new ZLinkFrameworkException(kind, "Instance factory failed.")
        );
        Assert.Equal(result, actual.Result);
        Assert.Equal(failureCode, (int)actual.FailureCode);
        Assert.True(
            ServiceWireConstants.ValidTerminalFailure((uint)actual.Result, (uint)actual.FailureCode)
        );
    }

    [Theory]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, "no_handler", true)]
    [InlineData(ZLinkFrameworkErrorKind.ProtocolError, "invalid_frame", true)]
    [InlineData(ZLinkFrameworkErrorKind.Unavailable, "stale_target", true)]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, "no_handler", false)]
    [InlineData(ZLinkFrameworkErrorKind.ProtocolError, "invalid_frame", false)]
    [InlineData(ZLinkFrameworkErrorKind.Unavailable, "stale_target", false)]
    public async Task Missing_Instance_factory_failure_keeps_kind_and_records_one_terminal(
        ZLinkFrameworkErrorKind kind,
        string reason,
        bool request
    )
    {
        await using var host = await SpotCloseHost.StartAsync();
        await host.RequestInstanceAsync($"factory-warm-{Guid.NewGuid():N}");
        await using var source = await StartPeerAsync(host, registerInstanceFactory: false);
        var spotId = $"factory-failed-{Guid.NewGuid():N}";
        var activities = new ConcurrentBag<Activity>();
        var diagnostic = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var listener = new ActivityListener
        {
            ShouldListenTo = activitySource =>
                activitySource.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activity =>
            {
                if (
                    activity.OperationName == "zlink.dispatch_error"
                    && Equals(activity.GetTagItem("spot_id"), spotId)
                )
                {
                    activities.Add(activity);
                    diagnostic.TrySetResult();
                }
            },
        };
        ActivitySource.AddActivityListener(listener);
        host.State.OnReinitialize = _ =>
            throw new ZLinkFrameworkException(kind, "Instance factory failed.");
        if (request)
        {
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                source.RequestInstanceAsync(spotId)
            );
            Assert.Equal(kind, failure.Kind);
        }
        else
        {
            await source.SendInstanceAsync(spotId, "factory-failed");
        }
        await diagnostic.Task.WaitAsync(Wait);
        var record = Assert.Single(activities);
        Assert.Equal("instance_spot", record.GetTagItem("surface"));
        Assert.Equal(reason, record.GetTagItem("reason"));
        Assert.Equal(request ? "reply_error" : "drop", record.GetTagItem("action"));
        Assert.Equal(request ? "request" : "send", record.GetTagItem("message_kind"));
        Assert.Equal(nameof(ZLinkFrameworkException), record.GetTagItem("error_type"));
        Assert.Equal("Instance factory failed.", record.GetTagItem("error_message"));
    }

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

    [Theory]
    [InlineData("Draining", ZLinkFrameworkErrorKind.ShuttingDown)]
    [InlineData("Relocating", ZLinkFrameworkErrorKind.Unavailable)]
    public async Task Accepted_intent_ends_when_Close_releases_authority_during_host_drain(
        string hostMode,
        ZLinkFrameworkErrorKind expectedKind
    )
    {
        var observed = await RunCloseBranchAsync(
            "release-during-host-drain-or-relocation",
            hostMode
        );
        Assert.Equal("Missing", observed.Authority);
        Assert.Equal(0, observed.OldHandlerCalls);
        Assert.Equal(0, observed.NewHandlerCalls);
        Assert.Equal(0, observed.FactoryCalls);
        Assert.Equal(expectedKind.ToString(), observed.Terminal);
        Assert.Equal(1, observed.TerminalCount);
    }

    [Theory]
    [InlineData("Draining", false, "shutdown")]
    [InlineData("Relocating", false, "stale_target")]
    [InlineData("Relocating", true, "stale_target")]
    public async Task Accepted_Ready_send_ends_in_diagnostics_without_replacement(
        string hostMode,
        bool sealedAdmission,
        string expectedReason
    ) => await RunAcceptedReadySendDiagnosticAsync(hostMode, sealedAdmission, expectedReason);

    private async Task<SendDiagnosticObservation> RunAcceptedReadySendDiagnosticAsync(
        string hostMode,
        bool sealedAdmission,
        string expectedReason
    )
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"release-ready-send-{hostMode}-{sealedAdmission}-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"release-ready-send-{Guid.NewGuid():N}";
        await host.RequestInstanceAsync(spotId);
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
            await host.SendInstanceAsync(spotId, "ready-send-released");
            await ObserveOwnerAcceptanceAsync(closing).WaitAsync(Wait);
            host.Runtime.DrainAdmission.BeginDrain(
                hostMode == "Draining" ? ZLinkDrainOwner.Shutdown : ZLinkDrainOwner.Relocation
            );
            if (sealedAdmission)
                host.Runtime.DrainAdmission.Seal();
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            await closing.PendingApplicationCompletion.WaitAsync(Wait);
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            Assert.DoesNotContain("ready-send-released", host.State.InstanceHandlerMarkers);
            var records = listener
                .ReadLines()
                .Where(line =>
                    line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal)
                );
            var record = Assert.Single(records);
            Assert.Contains("\"action\":\"drop\"", record);
            Assert.Contains($"\"reason\":\"{expectedReason}\"", record);
            var diagnostic = await ObserveSendDiagnosticAsync(listener);
            Assert.Equal("instance_spot", diagnostic.Surface);
            Assert.Equal(expectedReason, diagnostic.Reason);
            return diagnostic;
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
        }
    }

    [Theory]
    [InlineData("Draining", false, ZLinkFrameworkErrorKind.ShuttingDown)]
    [InlineData("Relocating", false, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData("Relocating", true, ZLinkFrameworkErrorKind.Unavailable)]
    public async Task Accepted_Ready_request_ends_without_replacement(
        string hostMode,
        bool sealedAdmission,
        ZLinkFrameworkErrorKind expectedKind
    )
    {
        var flowPath = Path.Combine(
            Path.GetTempPath(),
            "zlink-close-dotnet",
            $"release-ready-request-{hostMode}-{sealedAdmission}-{Guid.NewGuid():N}.flow"
        );
        using var listener = new TestHostMessageFlowListener(flowPath);
        output.WriteLine($"Message flow file: {flowPath}");
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"release-ready-request-{Guid.NewGuid():N}";
        await host.RequestInstanceAsync(spotId);
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

        var pending = host.RequestInstanceAsync(spotId, "ready-request-released");
        try
        {
            await ObserveOwnerAcceptanceAsync(closing).WaitAsync(Wait);
            host.Runtime.DrainAdmission.BeginDrain(
                hostMode == "Draining" ? ZLinkDrainOwner.Shutdown : ZLinkDrainOwner.Relocation
            );
            if (sealedAdmission)
                host.Runtime.DrainAdmission.Seal();
            host.State.ReleaseOnClosing.TrySetResult();
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                pending.WaitAsync(Wait)
            );
            Assert.Equal(expectedKind, failure.Kind);
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            await closing.PendingApplicationCompletion.WaitAsync(Wait);
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            Assert.DoesNotContain("ready-request-released", host.State.InstanceHandlerMarkers);
            var records = listener
                .ReadLines()
                .Where(line =>
                    line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal)
                );
            var record = Assert.Single(records);
            Assert.Contains("\"surface\":\"instance_spot\"", record);
            Assert.Contains("\"message_kind\":\"request\"", record);
            Assert.Contains("\"action\":\"reply_error\"", record);
            var expectedReason = hostMode == "Draining" ? "shutdown" : "stale_target";
            Assert.Contains($"\"reason\":\"{expectedReason}\"", record);
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
        }
    }

    [Theory]
    [InlineData(
        true,
        ZLinkFrameworkErrorKind.InvalidOperation,
        (int)ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
    )]
    [InlineData(
        false,
        ZLinkFrameworkErrorKind.Unavailable,
        (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving
    )]
    public async Task Close_owner_preserves_known_failure_code(
        bool staleGeneration,
        ZLinkFrameworkErrorKind kind,
        int code
    )
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-owner-cause-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        await using var otherOwner = await SpotCloseHost.StartAsync(host.Store);
        var catalog = otherOwner.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Catalog;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await catalog.CloseAsync(
                spotId,
                staleGeneration ? initial.Generation + 1 : initial.Generation,
                CancellationToken.None
            )
        );
        Assert.Equal(kind, error.Kind);
        var reply = ZLinkRequestFailureMapper.TargetFailureReply(error);
        Assert.Equal(RequestResult.Conflict, reply.Result);
        Assert.Equal((ServiceWireConstants.FrameworkErrorCode)code, reply.FailureCode);
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
        var operationId = node.AllocateOperationId();
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
            operationId,
            true,
            operationId.Low,
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

    // Close step 3 decides release with no Instance intent message waiting
    // while the Store holds its Delete. An Instance intent message arriving at
    // the owner in that window is refused before admission with the owner
    // fence code, so its caller re-reads authority instead of waiting for an
    // incarnation this Close never creates.
    private static async Task<CloseBranchObservation> RunIntentAfterReleaseDecisionAsync()
    {
        var order = new ConcurrentQueue<string>();
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-instance-{Guid.NewGuid():N}";
        var initial = await host.RequestInstanceAsync(spotId);
        host.State.HoldOnClosing = true;
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId);
        await host.State.OnClosingEntered.Task.WaitAsync(Wait);
        host.State.HandlerMode = null;
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
        host.Store.ObserveAuthorityReleaseFor = spotId;
        host.Store.OnAuthorityReleased = () => order.Enqueue("authorityReleased");
        host.Store.HoldAuthorityDeleteFor = spotId;
        var initializedBefore = host.State.InitializedGenerations.Count;
        host.State.ReleaseOnClosing.TrySetResult();
        // Step 3 has decided release: its Delete waits in the Store.
        await host.Store.AuthorityDeleteHeld.Task.WaitAsync(Wait);
        var late = QueueRetainedIntent(host, activation, current, spotId, "late");
        var deadline = DateTime.UtcNow + Wait;
        while (!late.IsCompleted && !activation.HasPendingCreationIntent)
        {
            if (DateTime.UtcNow > deadline)
                throw new TimeoutException("The late Instance intent message was not decided.");
            await Task.Delay(2);
        }
        host.Store.ReleaseAuthorityDelete.TrySetResult();
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => late.WaitAsync(Wait));
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
            failure.Kind.ToString(),
            1,
            host.Store.MissingPlacementCalls,
            false,
            false,
            false,
            false,
            false,
            failure.FrameworkFailureCode == (int)ServiceWireConstants.FrameworkErrorCode.SpotMoving
                ? "spotMoving"
                : failure.FrameworkFailureCode.ToString(
                    System.Globalization.CultureInfo.InvariantCulture
                )
        );
    }

    private static async Task<CloseBranchObservation> RunCloseBranchAsync(
        string name,
        string hostMode = "Serving",
        string relocationSeal = "before"
    )
    {
        if (name == "intent-after-release-decision-refused-before-admission")
            return await RunIntentAfterReleaseDecisionAsync();
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
        if (hostMode != "Serving")
        {
            host.Store.ObserveAuthorityReleaseFor = spotId;
            host.Store.OnAuthorityReleased = () => order.Enqueue("authorityReleased");
        }
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
        host.Store.ObserveMissingPlacementFor = spotId;
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
        if (hostMode == "Relocating" && relocationSeal == "after")
            host.Runtime.DrainAdmission.Seal();
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
            Assert.Equal(
                hostMode == "Draining"
                    ? ZLinkFrameworkErrorKind.ShuttingDown
                    : ZLinkFrameworkErrorKind.Unavailable,
                failure.Kind
            );
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
            order.Enqueue("pendingMessagesTerminated");
            terminalKind = failure.Kind.ToString();
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
            host.Store.MissingPlacementCalls,
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
        int MissingPlacementCalls,
        bool ObjectGenerationChanged,
        bool OwnerGenerationChanged,
        bool OwnerPreserved,
        bool LeasePreserved,
        bool CapacityPreserved,
        string? FailureCode = null
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
                    var seals =
                        mode == "Relocating"
                        && given.TryGetProperty("relocationSeal", out var sealValues)
                            ? sealValues
                                .EnumerateArray()
                                .Select(static item => item.GetString()!)
                                .ToArray()
                            : ["before"];
                    foreach (var seal in seals)
                    {
                        var observed = await RunCloseBranchAsync(name, mode, seal)
                            .WaitAsync(TimeSpan.FromSeconds(60));
                        var expected = branch.GetProperty("expect");
                        AssertCloseBranch(expected, observed, mode);
                        if (expected.TryGetProperty("sendDiagnosticsByHost", out var diagnostics))
                        {
                            var diagnostic = diagnostics.GetProperty(mode);
                            var observedDiagnostic = await RunAcceptedReadySendDiagnosticAsync(
                                mode,
                                seal == "after",
                                diagnostic.GetProperty("reason").GetString()!
                            );
                            Assert.Equal(
                                diagnostic.GetProperty("surface").GetString(),
                                observedDiagnostic.Surface
                            );
                            Assert.Equal(
                                diagnostic.GetProperty("reason").GetString(),
                                observedDiagnostic.Reason
                            );
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                failures.Add(
                    $"{name}: {exception.GetType().Name}: {exception.Message.ReplaceLineEndings(" ")}"
                );
            }
        }
        foreach (var routeCase in root.GetProperty("readyRouteCases").EnumerateArray())
        {
            try
            {
                foreach (
                    var intent in routeCase
                        .GetProperty("given")
                        .GetProperty("instanceIntent")
                        .EnumerateArray()
                )
                    await RunReadyRouteCaseAsync(routeCase, intent.GetBoolean())
                        .WaitAsync(TimeSpan.FromSeconds(60));
            }
            catch (Exception exception)
            {
                failures.Add(
                    $"{routeCase.GetProperty("name").GetString()}: {exception.Message.ReplaceLineEndings(" ")}"
                );
            }
        }
        Assert.True(failures.Count == 0, string.Join(Environment.NewLine, failures));
    }

    private static async Task<SpotCloseHost> StartPeerAsync(
        SpotCloseHost host,
        bool registerInstanceFactory = true
    )
    {
        var source = await SpotCloseHost.StartAsync(host.Store, registerInstanceFactory);
        using (var peerDeadline = new CancellationTokenSource(Wait))
        {
            while (true)
            {
                peerDeadline.Token.ThrowIfCancellationRequested();
                try
                {
                    source.Runtime.EnsureKnownRouteMeshPeer(
                        SpotCloseHost.MeshName,
                        host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.RoutingId,
                        "Instance Spot test peer"
                    );
                    host.Runtime.EnsureKnownRouteMeshPeer(
                        SpotCloseHost.MeshName,
                        source.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.RoutingId,
                        "Instance Spot test peer"
                    );
                    break;
                }
                catch (ZLinkFrameworkException error)
                    when (error.Kind == ZLinkFrameworkErrorKind.Unavailable)
                {
                    await Task.Yield();
                }
            }
        }
        return source;
    }

    private static async Task RunReadyRouteCaseAsync(JsonElement routeCase, bool instanceIntent)
    {
        var given = routeCase.GetProperty("given");
        var expected = routeCase.GetProperty("expect");
        Assert.Equal("mismatch", given.GetProperty("ownerFence").GetString());
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"stale-ready-{Guid.NewGuid():N}";
        var flowPath = Path.Combine(Path.GetTempPath(), "zlink-close-dotnet", $"{spotId}.flow");
        using var flow = new TestHostMessageFlowListener(flowPath);
        await host.RequestInstanceAsync(spotId);
        await using var source = await StartPeerAsync(host);
        var resolved = Assert.IsType<ZLinkResolvedSpotHandle>(
            await source.Runtime.ResolveSpotHandleAsync(spotId, CancellationToken.None)
        );
        var stale = resolved.Snapshot with
        {
            AuthorityOwnerGeneration = resolved.Snapshot.AuthorityOwnerGeneration + 1,
        };
        if (given.GetProperty("authority").GetString() == "Missing")
        {
            host.State.HandlerMode = "closeAndReturn";
            await host.RequestInstanceAsync(spotId);
            Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
            Assert.Equal("Missing", await host.AuthorityAsync(spotId));
        }
        else
            Assert.Equal("Ready", await host.AuthorityAsync(spotId));
        // A cold activation may land on either node; count both.
        int Handlers() => host.State.HandlerCalls + source.State.HandlerCalls;
        int Factories() =>
            host.State.InitializedGenerations.Count + source.State.InitializedGenerations.Count;
        var handlersBefore = Handlers();
        var factoriesBefore = Factories();
        var placementCalls = 0;
        var target = new ZLinkResolvedSpotHandle(
            stale,
            1,
            _ =>
            {
                placementCalls++;
                return ValueTask.FromResult<(ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>(
                    null
                );
            }
        );
        string? terminal = null;
        SendDiagnosticObservation? sendDiagnostic = null;
        var terminalCount = 0;
        if (
            given.GetProperty("messageKind").GetString() == "request"
            && instanceIntent
            && given.GetProperty("authority").GetString() == "Missing"
        )
        {
            // The source still caches the Ready route it resolved before Close
            // released authority; the public Instance intent call uses it.
            host.Store.ObserveMissingPlacementFor = spotId;
            var reply = await source.RequestInstanceAsync(spotId, "stale");
            Assert.Equal("stale", reply.Marker);
            terminal = "reply";
            terminalCount++;
            placementCalls = host.Store.MissingPlacementCalls;
        }
        else if (given.GetProperty("messageKind").GetString() == "request")
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                new ZLinkRouteSpotRequestCall<SpotCloseProbeRequest>(
                    source.Runtime,
                    target,
                    new("stale"),
                    instanceIntent
                )
                    .Async<SpotCloseInstanceReply>()
                    .AsTask()
            );
            terminal = error.Kind.ToString();
            terminalCount++;
        }
        else
        {
            await new ZLinkRouteSpotSendCall<SpotCloseProbeSignal>(
                source.Runtime,
                target,
                new("stale"),
                instanceIntent
            ).Async();
            sendDiagnostic = await ObserveSendDiagnosticAsync(flow);
        }
        var records = flow.ReadLines()
            .Where(line => line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal));
        var record = Assert.Single(records);
        var request = given.GetProperty("messageKind").GetString() == "request";
        Assert.Contains("\"surface\":\"instance_spot\"", record);
        Assert.Contains("\"reason\":\"stale_target\"", record);
        Assert.Contains(request ? "\"action\":\"reply_error\"" : "\"action\":\"drop\"", record);
        Assert.Contains(
            request ? "\"message_kind\":\"request\"" : "\"message_kind\":\"send\"",
            record
        );
        foreach (var field in expected.EnumerateObject())
        {
            switch (field.Name)
            {
                case "messageTerminal":
                    Assert.Equal(field.Value.GetString(), terminal);
                    break;
                case "messageTerminalCount":
                    Assert.Equal(field.Value.GetInt32(), terminalCount);
                    break;
                case "diagnostics":
                    Assert.NotNull(sendDiagnostic);
                    Assert.Equal(1, field.Value.GetArrayLength());
                    break;
                case "surface":
                    Assert.Equal(field.Value.GetString(), sendDiagnostic!.Surface);
                    break;
                case "reason":
                    Assert.Equal(field.Value.GetString(), sendDiagnostic!.Reason);
                    break;
                case "handlerCalls":
                    Assert.Equal(field.Value.GetInt32(), Handlers() - handlersBefore);
                    break;
                case "factoryCalls":
                    Assert.Equal(field.Value.GetInt32(), Factories() - factoriesBefore);
                    break;
                case "missingPlacementCalls":
                    Assert.Equal(field.Value.GetInt32(), placementCalls);
                    break;
                default:
                    throw new Xunit.Sdk.XunitException(
                        $"Unknown Ready route expectation '{field.Name}'."
                    );
            }
        }
    }

    private sealed record SendDiagnosticObservation(string Surface, string Reason);

    private static async Task<SendDiagnosticObservation> ObserveSendDiagnosticAsync(
        TestHostMessageFlowListener listener
    )
    {
        using var deadline = new CancellationTokenSource(Wait);
        while (true)
        {
            deadline.Token.ThrowIfCancellationRequested();
            foreach (var line in listener.ReadLines())
            {
                if (!line.Contains("event=zlink.dispatch_error", StringComparison.Ordinal))
                    continue;
                var tagsOffset = line.IndexOf(" tags=", StringComparison.Ordinal);
                using var tags = JsonDocument.Parse(line[(tagsOffset + " tags=".Length)..]);
                Assert.Equal(
                    nameof(ZLinkFrameworkException),
                    tags.RootElement.GetProperty("error_type").GetString()
                );
                return new SendDiagnosticObservation(
                    tags.RootElement.GetProperty("surface").GetString()!,
                    tags.RootElement.GetProperty("reason").GetString()!
                );
            }
            await Task.Yield();
        }
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

    // Failover policy §4.4: once Close has released authority, the next
    // Instance intent request cold-activates a new incarnation even when the
    // caller still holds the Ready route it cached before the Close.
    [Fact]
    public async Task Instance_intent_request_on_a_route_cached_before_Close_cold_activates()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"cached-close-{Guid.NewGuid():N}";
        var first = await host.RequestInstanceAsync(spotId);
        await using var source = await StartPeerAsync(host);
        Assert.NotNull(await source.Runtime.ResolveSpotHandleAsync(spotId, CancellationToken.None));
        host.State.HandlerMode = "closeAndReturn";
        await host.RequestInstanceAsync(spotId, "close");
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        Assert.Equal("Missing", await host.AuthorityAsync(spotId));
        host.State.HandlerMode = null;

        var reply = await source.RequestInstanceAsync(spotId, "after-close").WaitAsync(Wait);

        Assert.Equal("after-close", reply.Marker);
        Assert.NotEqual(first.Generation, reply.Generation);
        Assert.Equal("Ready", await host.AuthorityAsync(spotId));
    }

    // Spot messaging §4 step 12 and §7 step 1: a Close the first handler
    // requests is the next lifecycle item, so it starts only after the first
    // terminal is durably recorded. Holding that record proves the order: the
    // Close must not commit Closing while the record is held.
    [Fact]
    public async Task Cold_activation_records_its_first_terminal_before_a_handler_requested_close()
    {
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"activation-close-{Guid.NewGuid():N}";
        host.Store.HoldReplayCursorFor = spotId;
        host.State.HandlerMode = "closeAndReturn";

        var request = host.RequestInstanceAsync(spotId);
        try
        {
            await host.Store.ReplayCursorHeld.Task.WaitAsync(Wait);
            var closedWhileHeld = await Task.WhenAny(
                host.State.OnClosingEntered.Task,
                Task.Delay(TimeSpan.FromMilliseconds(500))
            );
            Assert.NotSame(host.State.OnClosingEntered.Task, closedWhileHeld);
        }
        finally
        {
            host.Store.ReleaseReplayCursor.TrySetResult();
        }

        var reply = await request.WaitAsync(Wait);
        Assert.Equal("initial", reply.Marker);
        var close = Assert.IsAssignableFrom<Task<bool>>(host.State.ContextCloseTask);
        Assert.True(await close.WaitAsync(Wait));
        Assert.Equal("Missing", await host.AuthorityAsync(spotId));
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

    private static void AssertCloseBranch(
        JsonElement expected,
        CloseBranchObservation observed,
        string hostMode
    )
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
                case "sendDiagnosticsByHost":
                    Assert.Equal(
                        observed.Terminal,
                        field.Value.GetProperty(hostMode).GetProperty("kind").GetString()
                    );
                    break;
                case "missingPlacementCalls":
                    Assert.Equal(field.Value.GetInt32(), observed.MissingPlacementCalls);
                    break;
                case "messageTerminalByHost":
                    Assert.Equal(field.Value.GetProperty(hostMode).GetString(), observed.Terminal);
                    break;
                case "messageTerminal":
                    Assert.Equal(field.Value.GetString(), observed.Terminal);
                    break;
                case "messageTerminalCount":
                    Assert.Equal(field.Value.GetInt32(), observed.TerminalCount);
                    break;
                case "messageFailureCode":
                    Assert.Equal(field.Value.GetString(), observed.FailureCode);
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

    internal static async Task<SpotCloseHost> StartAsync(
        SpotCloseFaultStore? sharedStore = null,
        bool registerInstanceFactory = true
    )
    {
        var store =
            sharedStore ?? new SpotCloseFaultStore(new ZLinkInMemoryProviderLocationStore());
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
            var server = options
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
                );
            if (registerInstanceFactory)
                server.AddInstanceSpotFactory<SpotCloseInstance>(
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
    internal string? ObserveMissingPlacementFor { get; set; }
    internal string? ObserveAuthorityReleaseFor { get; set; }
    internal Action? OnAuthorityReleased { get; set; }

    // Holds the cold-activation replay cursor write (spot messaging §4 step 12)
    // for one Spot until the test releases it.
    internal string? HoldReplayCursorFor { get; set; }

    // Holds the authority Delete of one Spot until the test releases it.
    internal string? HoldAuthorityDeleteFor { get; set; }
    internal TaskCompletionSource AuthorityDeleteHeld { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource ReleaseAuthorityDelete { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource ReplayCursorHeld { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal TaskCompletionSource ReleaseReplayCursor { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private int _missingPlacementCalls;
    internal int MissingPlacementCalls => Volatile.Read(ref _missingPlacementCalls);

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
            HoldReplayCursorFor is { } heldSpot
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Put put
                && put.Key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                && put.Key.Value.EndsWith("\0" + heldSpot, StringComparison.Ordinal)
                && WritesCompletedReplayCursor(put.Bytes.Span)
            )
        )
        {
            ReplayCursorHeld.TrySetResult();
            await ReleaseReplayCursor.Task.WaitAsync(cancellationToken);
        }
        if (
            HoldAuthorityDeleteFor is { } deletedSpot
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Delete delete
                && delete.Key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                && delete.Key.Value.EndsWith("\0" + deletedSpot, StringComparison.Ordinal)
            )
        )
        {
            AuthorityDeleteHeld.TrySetResult();
            await ReleaseAuthorityDelete.Task.WaitAsync(cancellationToken);
        }
        if (
            ObserveMissingPlacementFor is { } observedSpot
            && request.Conditions.Any(condition =>
                condition is ZLinkStoreCondition.Missing missing
                && missing.Key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                && missing.Key.Value.EndsWith("\0" + observedSpot, StringComparison.Ordinal)
            )
        )
            Interlocked.Increment(ref _missingPlacementCalls);
        if (
            ConflictPutsFor is { } conflict
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Put put && put.Key.Value.Contains(conflict)
            )
        )
        {
            ConflictPutsFor = null;
            var put = request
                .Mutations.OfType<ZLinkStoreMutation.Put>()
                .Single(put =>
                    put.Key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                    && put.Key.Value.EndsWith("\0" + conflict, StringComparison.Ordinal)
                );
            var current = Assert.IsType<ZLinkStoreReadResult.Found>(
                await inner.ReadAsync(put.Key, cancellationToken)
            );
            var rewritten = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await inner.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [],
                        [new ZLinkStoreMutation.Put(put.Key, current.Value.Bytes, null)]
                    ),
                    cancellationToken
                )
            );
            Assert.NotEqual(current.Value.Version, rewritten.PutVersions[put.Key]);
            return Assert.IsType<ZLinkStoreWriteResult.Conflict>(
                await inner.WriteAsync(request, cancellationToken)
            );
        }
        var written = await inner.WriteAsync(request, cancellationToken);
        if (
            written is ZLinkStoreWriteResult.Applied
            && ObserveAuthorityReleaseFor is { } releasedSpot
            && request.Mutations.Any(mutation =>
                mutation is ZLinkStoreMutation.Delete delete
                && delete.Key.Value.StartsWith("authority\0", StringComparison.Ordinal)
                && delete.Key.Value.EndsWith("\0" + releasedSpot, StringComparison.Ordinal)
            )
        )
            OnAuthorityReleased?.Invoke();
        return written;
    }

    // The provider authority record is JSON whose payload field is base64;
    // any string field that decodes as an Instance authority payload is it.
    private static bool WritesCompletedReplayCursor(ReadOnlySpan<byte> record)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(record.ToArray());
        }
        catch (JsonException)
        {
            return false;
        }
        using (document)
            return document
                .RootElement.EnumerateObject()
                .Any(static property =>
                    property.Value.ValueKind == JsonValueKind.String
                    && property.Value.TryGetBytesFromBase64(out var bytes)
                    && ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(bytes, out var payload)
                    && payload
                        is {
                            State: ZLinkInstanceSpotAuthorityState.Ready,
                            ActivationRecovery: { InboxSequence: > 0 } recovery,
                        }
                    && recovery.ReplayCursor == recovery.InboxSequence
                );
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
