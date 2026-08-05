using System.Runtime.CompilerServices;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

public sealed class FrameworkRuntimeContracts
{
    [Fact]
    public void Public_values_match_the_exact_contract()
    {
        Assert.Equal(2, (int)ZLinkFrameworkRuntimeState.Relocating);
        Assert.Equal(3, (int)ZLinkFrameworkRuntimeState.Relocated);
        Assert.Equal(8, (int)ZLinkFrameworkRelocationReason.ManualTopologyUnsupported);
        Assert.Equal(10, (int)ZLinkFrameworkRelocationReason.OperationInProgress);
    }

    [Fact]
    [ContractExample(typeof(IZLinkFrameworkRuntime))]
    public async Task Relocation_and_shutdown_are_separate_host_operations()
    {
        var runtime = new ExampleFrameworkRuntime();

        var relocated = await runtime.RelocateAsync(new ZLinkFrameworkRelocationOptions
        {
            Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
            TargetApplicationVersion = 8,
            Deadline = TimeSpan.FromSeconds(30)
        });

        Assert.Equal(ZLinkFrameworkRelocationOutcome.Relocated, relocated.Outcome);
        Assert.Equal(ZLinkFrameworkRuntimeState.Relocated, runtime.Status.State);
        Assert.Null(runtime.Status.TerminationResult);

        var stopped = await runtime.ShutdownAsync();

        Assert.Equal(ZLinkFrameworkTerminationOutcome.Stopped, stopped.Outcome);
        Assert.Equal(ZLinkFrameworkRuntimeState.Stopped, runtime.Status.State);
        Assert.Equal(stopped, runtime.Status.TerminationResult);
    }

    [Fact]
    public void Retire_surface_is_not_public()
    {
        var contract = typeof(IZLinkFrameworkRuntime);

        Assert.NotNull(contract.GetProperty(nameof(IZLinkFrameworkRuntime.Status)));
        Assert.NotNull(contract.GetMethod(nameof(IZLinkFrameworkRuntime.RelocateAsync)));
        Assert.NotNull(contract.GetMethod(nameof(IZLinkFrameworkRuntime.ShutdownAsync)));
        Assert.Null(contract.GetMethod("RetireAsync"));
        Assert.Null(contract.GetMethod("DrainAsync"));
        Assert.Null(contract.GetMethod("AwaitDrainedAsync"));
    }

    private sealed class ExampleFrameworkRuntime : IZLinkFrameworkRuntime
    {
        private ulong _sequence;

        public ZLinkFrameworkRuntimeStatus Status { get; private set; } = Create(
            ZLinkFrameworkRuntimeState.Serving,
            sequence: 0);

        public async IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> ObserveAsync(
            [EnumeratorCancellation] CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            yield return new ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>(
                Status,
                default);
            await Task.CompletedTask;
        }

        public ValueTask<ZLinkFrameworkRelocationResult> RelocateAsync(
            ZLinkFrameworkRelocationOptions options,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var target = options.Mode == ZLinkFrameworkRelocationMode.RollingUpdate
                ? options.TargetApplicationVersion
                  ?? throw new ArgumentException("A target version is required.", nameof(options))
                : 7;
            var result = new ZLinkFrameworkRelocationResult(
                options.Mode,
                target,
                ZLinkFrameworkRelocationOutcome.Relocated,
                ZLinkFrameworkRelocationReason.None);
            Status = Create(
                ZLinkFrameworkRuntimeState.Relocated,
                checked(++_sequence),
                relocation: result);
            return ValueTask.FromResult(result);
        }

        public ValueTask<ZLinkFrameworkTerminationResult> ShutdownAsync(
            TimeSpan? deadline = null,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = new ZLinkFrameworkTerminationResult(
                ZLinkFrameworkTerminationOutcome.Stopped,
                ZLinkFrameworkTerminationReason.None);
            Status = Create(
                ZLinkFrameworkRuntimeState.Stopped,
                checked(++_sequence),
                relocation: Status.RelocationResult,
                termination: result);
            return ValueTask.FromResult(result);
        }

        private static ZLinkFrameworkRuntimeStatus Create(
            ZLinkFrameworkRuntimeState state,
            ulong sequence,
            ZLinkFrameworkRelocationResult? relocation = null,
            ZLinkFrameworkTerminationResult? termination = null) =>
            new(
                state,
                state == ZLinkFrameworkRuntimeState.Serving,
                state == ZLinkFrameworkRuntimeState.Serving,
                null,
                relocation,
                termination,
                sequence,
                DateTimeOffset.UtcNow);
    }
}
