using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task Actor_Request_Deadline_IncludesAuthorityRead_AndPreventsLateSubmit()
    {
        const string relative =
            "framework/runtime/conformance/actor-request-lookup-deadline-v1.json";
        var root = new DirectoryInfo(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, relative)))
            root = root.Parent;
        Assert.NotNull(root);
        using var fixture = System.Text.Json.JsonDocument.Parse(
            File.ReadAllText(Path.Combine(root.FullName, relative))
        );
        var node = new CapturingSpotNode();
        HeldActorAuthorityStore? held = null;
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            locationStoreWrapper: store => held = new HeldActorAuthorityStore(store)
        );
        try
        {
            held!.Hold = true;
            var caller = new ZLinkActorClient(runtime)
                .RequestToActor(actor.ActorId, new ProbeRouteMessage("held"))
                .Timeout(
                    TimeSpan.FromMilliseconds(
                        fixture.RootElement.GetProperty("timeoutMs").GetInt32()
                    )
                )
                .Async<ProbeRouteMessage>()
                .AsTask();
            try
            {
                await held.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    caller.WaitAsync(
                        TimeSpan.FromMilliseconds(
                            fixture.RootElement.GetProperty("observationMs").GetInt32()
                        )
                    )
                );
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
                Assert.False(held.Release.Task.IsCompleted);
            }
            finally
            {
                held.Release.TrySetResult();
            }
            await held.Returned.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await Task.Yield();
            Assert.Empty(node.ActorRequests);
        }
        finally
        {
            held?.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class HeldActorAuthorityStore(IZLinkLocationStore inner) : IZLinkLocationStore
    {
        internal bool Hold;
        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Returned { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken ct = default
        )
        {
            if (Hold && key.Value == "authority\0actor\0actor-a")
            {
                Entered.TrySetResult();
                await Release.Task;
                try
                {
                    return await inner.ReadAsync(key, ct);
                }
                finally
                {
                    Returned.TrySetResult();
                }
            }
            return await inner.ReadAsync(key, ct);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken ct = default
        ) => inner.WriteAsync(request, ct);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken ct = default
        ) => inner.ScanAsync(request, ct);
    }
}
