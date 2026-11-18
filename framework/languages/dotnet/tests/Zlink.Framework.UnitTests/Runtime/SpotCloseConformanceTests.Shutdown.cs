using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class SpotCloseConformanceTests
{
    [Fact]
    public async Task Context_Close_retains_host_admission_after_its_lifecycle_record()
    {
        using var flow = ClosingFlow(
            nameof(Context_Close_retains_host_admission_after_its_lifecycle_record),
            true,
            0
        );
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-admission-terminal-{Guid.NewGuid():N}";
        IZLinkInstanceSpotContext? context = null;
        host.State.OnInstanceInitializing = current => context = current;
        var terminalAdmission = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        host.State.OnInstanceClosing = _ =>
            ZLinkSerialTurn.Current!.Item.AddTerminalRelease(() =>
                terminalAdmission.TrySetResult(
                    host.Runtime.SnapshotOperationAdmissions().ActiveCount
                )
            );

        await host.RequestInstanceAsync(spotId);
        await ((ZLinkSpotActivation)context!)._serial.ExecuteAsync(
            (_, _) =>
            {
                using var operation = ZLinkFrameworkRuntime.EnterAcceptedOperation(null);
                host.State.ContextCloseTask = context.CloseAsync().AsTask();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        var active = await terminalAdmission.Task.WaitAsync(Wait);
        output.WriteLine($"Host admissions at Close record terminal: {active}");
        Assert.Equal(1, active);
        Assert.Equal(0, host.Runtime.SnapshotOperationAdmissions().ActiveCount);
    }

    [Fact]
    public async Task Context_Close_refused_by_shutdown_leaves_no_host_admission()
    {
        using var flow = ClosingFlow(
            nameof(Context_Close_refused_by_shutdown_leaves_no_host_admission),
            true,
            0
        );
        await using var host = await SpotCloseHost.StartAsync();
        var catalog = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Catalog;
        IZLinkInstanceSpotContext? context = null;
        host.State.OnInstanceInitializing = current => context = current;
        await host.RequestInstanceAsync($"close-admission-refused-{Guid.NewGuid():N}");
        await ((ZLinkSpotActivation)context!)._serial.ExecuteAsync(
            (_, _) =>
            {
                host.Runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
                using var operation = ZLinkFrameworkRuntime.EnterAcceptedOperation(null);
                host.State.ContextCloseTask = context.CloseAsync().AsTask();
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            host.State.ContextCloseTask!.WaitAsync(Wait)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
        Assert.Equal(0, host.State.OnClosingCalls);
        Assert.Equal(0, host.Runtime.SnapshotOperationAdmissions().ActiveCount);
        Assert.Equal(0, await catalog.CloseTransactionCountAsync());
    }

    [Fact]
    public async Task Shutdown_started_during_context_Close_successor_initialization_closes_successor()
    {
        using var flow = ClosingFlow(
            nameof(Shutdown_started_during_context_Close_successor_initialization_closes_successor),
            true,
            0
        );
        await using var host = await SpotCloseHost.StartAsync();
        var spotId = $"close-successor-shutdown-{Guid.NewGuid():N}";
        IZLinkInstanceSpotContext? current = null;
        host.State.OnInstanceInitializing = context => current = context;
        var initial = await host.RequestInstanceAsync(spotId);
        var authority = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await host
                    .Runtime.Registration.Locations.ResolveStore()!
                    .ReadAuthorityAsync(ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId))
            )
            .Snapshot;
        Task<ZLinkFrameworkTerminationResult>? shutdown = null;
        Task<InstanceSpotActivationTerminal>? retained = null;
        host.State.OnInstanceClosing = generation =>
            host.State.Order.Enqueue(
                generation.ToString(System.Globalization.CultureInfo.InvariantCulture)
            );

        host.State.OnInstanceInitializing = context =>
        {
            current = context;
            if (context.ObjectGeneration != initial.Generation)
                shutdown = host
                    .Runtime.Services.GetRequiredService<IZLinkFrameworkRuntime>()
                    .ShutdownAsync()
                    .AsTask();
        };
        // Run the context call on its application gate so host termination
        // does not race an unrelated remote trigger request's reply.
        await ((ZLinkSpotActivation)current!)._serial.ExecuteAsync(
            async (activation, _) =>
            {
                // Target acceptance keeps this intent pending behind the current
                // handler turn; source send completion does not establish that.
                retained = await AdmitRetainedIntentAsync(
                    host,
                    activation,
                    authority,
                    spotId,
                    "successor-intent"
                );
                Assert.False(retained.IsCompleted);
                host.State.ContextCloseTask = current!.CloseAsync().AsTask();
            },
            CancellationToken.None
        );
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        Assert.NotNull(shutdown);
        var stopped = await shutdown.WaitAsync(Wait);
        output.WriteLine(
            $"Shutdown outcome: {stopped.Outcome}; initialized={string.Join(",", host.State.InitializedGenerations)}; closed={string.Join(",", host.State.Order)}"
        );
        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, stopped.Outcome);
        Assert.NotNull(retained);
        Assert.Equal(RequestResult.Ok, (await retained.WaitAsync(Wait)).Result);
        Assert.Equal(2, host.State.InitializedGenerations.Count);
        Assert.Equal(
            host.State.InitializedGenerations.Select(generation =>
                    generation.ToString(System.Globalization.CultureInfo.InvariantCulture)
                )
                .Order(),
            host.State.Order.Order()
        );
        Assert.Equal("Missing", await host.AuthorityAsync(spotId));
    }
}
