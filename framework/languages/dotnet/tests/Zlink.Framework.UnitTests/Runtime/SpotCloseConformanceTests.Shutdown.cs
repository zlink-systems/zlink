using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;

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
        host.State.OnInstanceInitializing = context =>
        {
            // Isolate the record's admission from the creating request's ambient lease.
            using var operation = ZLinkFrameworkRuntime.EnterAcceptedOperation(null);
            host.State.ContextCloseTask = context.CloseAsync().AsTask();
        };
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
        host.State.OnInstanceInitializing = context =>
        {
            host.Runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            using var operation = ZLinkFrameworkRuntime.EnterAcceptedOperation(null);
            host.State.ContextCloseTask = context.CloseAsync().AsTask();
        };

        await host.RequestInstanceAsync($"close-admission-refused-{Guid.NewGuid():N}");
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
        await using var source = await StartPeerAsync(host, registerInstanceFactory: false);
        Task<ZLinkFrameworkTerminationResult>? shutdown = null;
        host.State.OnInstanceHandler = async _ =>
        {
            host.State.OnInstanceHandler = null;
            host.State.ContextCloseTask = current!.CloseAsync().AsTask();
            await source.SendInstanceAsync(spotId, "successor-intent");
        };
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
        await source.RequestInstanceAsync(spotId, "close");
        Assert.True(await host.State.ContextCloseTask!.WaitAsync(Wait));
        Assert.NotNull(shutdown);
        var stopped = await shutdown.WaitAsync(Wait);
        output.WriteLine(
            $"Shutdown outcome: {stopped.Outcome}; initialized={string.Join(",", host.State.InitializedGenerations)}; closed={string.Join(",", host.State.Order)}"
        );
        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, stopped.Outcome);
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
