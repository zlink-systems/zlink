using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class PreReadyLogicalMulticastTests
{
    private const string MeshName = "pre-ready";
    private const string Topic = "events";
    private const string SpotId = "pre-ready-spot";
    private const string PublishedMessage = "retained";
    private const string InitializationFailureMessage = "initialization failed";
    private const string ReadyStoreFailureMessage = "Ready store failed";
    private static readonly TimeSpan Hang = TimeSpan.FromSeconds(10);

    [Theory]
    [InlineData(false, Outcome.Accept, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, Outcome.Accept, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(false, Outcome.Reject, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, Outcome.InitializeFailure, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(true, Outcome.StoreFailure, ZLinkUserSpotExecutionMode.SpotWide)]
    [InlineData(false, Outcome.Accept, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, Outcome.Accept, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(false, Outcome.Reject, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, Outcome.InitializeFailure, ZLinkUserSpotExecutionMode.PerActor)]
    [InlineData(true, Outcome.StoreFailure, ZLinkUserSpotExecutionMode.PerActor)]
    public async Task Matched_publish_waits_for_Ready_or_is_disposed(
        bool holdInitialize,
        Outcome outcome,
        ZLinkUserSpotExecutionMode executionMode
    )
    {
        var probe = new Probe(holdInitialize, outcome);
        var store = new ReadyStore(probe);
        var services = new ServiceCollection();
        services.AddSingleton(probe);
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRelocationStore(new InMemoryRelocationStore());
            var mesh = options.AddRouteMesh(MeshName).Listen("tcp://127.0.0.1:0");
            mesh.Channel(MeshName).Server();
            mesh.Objects()
                .Server()
                .AddSpotFactory<CreatingSpot>(
                    "pre-ready-spot-type",
                    factory => factory.ExecutionMode(executionMode).RecreateOnRelocation()
                );
        });
        await using var provider = services.BuildServiceProvider();
        var hosted = provider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService);
        await hosted.StartAsync(CancellationToken.None);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var node = Assert.IsType<ZLinkBackendSpotNodeWrapper>(
            runtime.GetSpotNodeRuntime(MeshName).Node
        );
        var creation = runtime.GetOrCreateAsync<CreatingSpot>(SpotId).AsTask();
        try
        {
            await probe.Entered.Task.WaitAsync(Hang);
            var mailbox = node.Pump.RegisterSpot(SpotId);
            await provider
                .GetRequiredService<IZLinkSpotPublisherClient>()
                .Publish(MeshName, Topic, PublishedMessage)
                .Async();
            Assert.True(SpinWait.SpinUntil(() => !mailbox.Subscriptions.IsEmpty, Hang));
            Assert.True(mailbox.Subscriptions.TryPeek(out var retained));
            var parts = retained!.Parts.ToArray();
            var admission = retained.ApplicationJobAdmission;
            Assert.NotNull(admission);
            Assert.NotEmpty(parts);
            Assert.Equal(0, probe.Handled);
            probe.Release.TrySetResult();

            if (outcome is Outcome.Accept or Outcome.StoreFailure)
            {
                await probe.StoreEntered.Task.WaitAsync(Hang);
                // Initialization has completed, but the Location Store has not committed Ready.
                Assert.Equal(0, probe.Handled);
                probe.StoreRelease.TrySetResult();
            }

            if (outcome == Outcome.InitializeFailure)
            {
                var failure = await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    creation.WaitAsync(Hang)
                );
                Assert.Equal(InitializationFailureMessage, failure.Message);
            }
            else if (outcome == Outcome.StoreFailure)
            {
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    creation.WaitAsync(Hang)
                );
                Assert.Contains(ReadyStoreFailureMessage, failure.ToString());
            }
            else
            {
                var result = await creation.WaitAsync(Hang);
                Assert.Equal(
                    outcome == Outcome.Accept
                        ? ZLinkSpotCreateState.Created
                        : ZLinkSpotCreateState.Rejected,
                    result.State
                );
            }

            if (outcome == Outcome.Accept)
            {
                Assert.Equal(PublishedMessage, await probe.Delivered.Task.WaitAsync(Hang));
                Assert.Equal(1, probe.Handled);
            }
            else
                Assert.Equal(0, probe.Handled);
            Assert.True(
                SpinWait.SpinUntil(
                    () => mailbox.Subscriptions.IsEmpty && admission.IsReleased,
                    Hang
                )
            );
            Assert.True(SpinWait.SpinUntil(() => parts.All(IsDisposed), Hang));
            Assert.All(parts, part => Assert.Throws<ObjectDisposedException>(() => part.Size));
            if (outcome == Outcome.Accept)
            {
                await provider
                    .GetRequiredService<IZLinkSpotPublisherClient>()
                    .Publish(MeshName, Topic, PublishedMessage)
                    .Async();
                Assert.True(SpinWait.SpinUntil(() => Volatile.Read(ref probe.Handled) == 2, Hang));
                Assert.Equal(2, probe.Handled);
            }
        }
        finally
        {
            probe.Release.TrySetResult();
            probe.StoreRelease.TrySetResult();
            try
            {
                await creation.WaitAsync(Hang);
            }
            catch (ZLinkFrameworkException) { }
            catch (InvalidOperationException) when (outcome == Outcome.InitializeFailure) { }
            await hosted.StopAsync(CancellationToken.None).WaitAsync(Hang);
        }
    }

    public enum Outcome
    {
        Accept,
        Reject,
        InitializeFailure,
        StoreFailure,
    }

    private static bool IsDisposed(Message part)
    {
        try
        {
            _ = part.Size;
            return false;
        }
        catch (ObjectDisposedException)
        {
            return true;
        }
    }

    private sealed class Probe(bool holdInitialize, Outcome outcome)
    {
        internal bool HoldInitialize => holdInitialize;
        internal Outcome Outcome => outcome;
        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource StoreEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource StoreRelease { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource<string> Delivered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int Handled;

        internal async ValueTask HoldAsync()
        {
            Entered.TrySetResult();
            await Release.Task;
        }
    }

    private sealed class CreatingSpot(IZLinkSpotContext context, Probe probe) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure() =>
            Context.Handlers.AddSubscribe<SubscriptionHandler>(MeshName, Topic);

        public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            if (!probe.HoldInitialize)
                await probe.HoldAsync();
            return probe.Outcome == Outcome.Reject
                ? ZLinkSpotCreateResponse.Reject()
                : ZLinkSpotCreateResponse.Accept();
        }

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            if (probe.HoldInitialize)
                await probe.HoldAsync();
            if (probe.Outcome == Outcome.InitializeFailure)
                throw new InvalidOperationException(InitializationFailureMessage);
        }
    }

    private sealed class SubscriptionHandler(Probe probe)
        : IZLinkSpotSubscriptionHandler<CreatingSpot, string>
    {
        public ValueTask HandleAsync(
            CreatingSpot spot,
            string message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken
        )
        {
            Interlocked.Increment(ref probe.Handled);
            probe.Delivered.TrySetResult(message);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ReadyStore(Probe probe) : IZLinkLocationStore
    {
        private readonly IZLinkLocationStore _inner = new ZLinkInMemoryProviderLocationStore();
        private int _readyWrites;

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => _inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => _inner.ScanAsync(request, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                probe.Release.Task.IsCompleted
                && request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key.Value.Contains(SpotId, StringComparison.Ordinal))
                && Interlocked.Increment(ref _readyWrites) == 1
            )
            {
                probe.StoreEntered.TrySetResult();
                await probe.StoreRelease.Task.WaitAsync(cancellationToken);
                if (probe.Outcome == Outcome.StoreFailure)
                    throw new InvalidOperationException(ReadyStoreFailureMessage);
            }
            return await _inner.WriteAsync(request, cancellationToken);
        }
    }
}
