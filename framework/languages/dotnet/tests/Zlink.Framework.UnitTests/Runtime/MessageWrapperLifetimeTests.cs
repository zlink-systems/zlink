using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class MessageWrapperLifetimeTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Finalizer_detector_distinguishes_leaked_and_closed_wrappers(bool dispose)
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Finalizer_detector_distinguishes_leaked_and_closed_wrappers)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(() =>
        {
            MessageFinalizerProbe.CreateMessages(dispose);
            return Task.CompletedTask;
        });
        output.WriteLine($"dispose={dispose}; finalizers={count}");
        Assert.Equal(dispose ? 0 : 16, count);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    public async Task Fallback_terminal_closes_wrapper(bool cancel, bool reject)
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Fallback_terminal_closes_wrapper)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(async () =>
        {
            using var cancellation = new CancellationTokenSource();
            if (cancel)
                cancellation.Cancel();
            var stream = new FallbackStream(reject);
            for (var index = 0; index < 16; index++)
            {
                var submit = () =>
                    ZLinkSessionStreamTransport.SubmitAsync(
                        stream,
                        Message.From("fallback"),
                        cancellation.Token
                    );
                if (cancel)
                    await Assert.ThrowsAnyAsync<OperationCanceledException>(submit);
                else if (reject)
                    await Assert.ThrowsAsync<ZlinkSubmitException>(submit);
                else
                    await submit();
            }
        });
        output.WriteLine($"cancel={cancel}; reject={reject}; finalizers={count}");
        Assert.Equal(0, count);
    }

    [Fact]
    public async Task Empty_actor_join_fallback_closes_its_wrapper()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Empty_actor_join_fallback_closes_its_wrapper)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(() =>
        {
            for (var index = 0; index < 16; index++)
            {
                using var batch = new MeshReceiveBatch();
                batch.Add(
                    new MeshReceiveRecord(
                        MeshRecordKind.ActorRequest,
                        MeshReadyDomains.Application,
                        RoutingId.From("join-source"),
                        "spot",
                        1,
                        new ActorRef("actor", 1, "mesh", RoutingId.From("owner")),
                        new MeshOperationId(1, 1),
                        MeshOperationKind.ActorRequest,
                        channelName: null,
                        topic: null,
                        applicationMetadata: null,
                        partOffset: 0,
                        partCount: 0,
                        terminalResult: 0,
                        failureErrno: 0,
                        kindData: null
                    ),
                    Array.Empty<Message>()
                );
                using var request = ZLinkMeshRecordAdapters.ToActorJoinRequest(batch, 0, batch[0]);
                Assert.Same(request.Message, Assert.Single(request.Parts));
                Assert.True(request.Message.IsEmpty);
            }
            return Task.CompletedTask;
        });
        output.WriteLine($"empty actor join finalizers={count}");
        Assert.Equal(0, count);
    }

    [Fact]
    public async Task Partial_creation_failure_rolls_back_every_created_wrapper()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Partial_creation_failure_rolls_back_every_created_wrapper)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(() =>
        {
            for (var attempt = 0; attempt < 16; attempt++)
            {
                var created = new List<Message>();
                var failure = new InvalidOperationException("partial-creation");
                Assert.Same(
                    failure,
                    Assert.Throws<InvalidOperationException>(() =>
                        ZLinkRawRouterServicePort.CreateMessages(
                            new[] { 0, 1, 2 },
                            index =>
                            {
                                if (index == 2)
                                    throw failure;
                                var part = Message.From("part");
                                created.Add(part);
                                return part;
                            }
                        )
                    )
                );
                Assert.Equal(2, created.Count);
                Assert.All(
                    created,
                    part => Assert.Throws<ObjectDisposedException>(() => part.Size)
                );
            }
            return Task.CompletedTask;
        });
        output.WriteLine($"partial creation rollback finalizers={count}");
        Assert.Equal(0, count);
    }

    [Fact]
    public async Task Disposed_binding_receive_wrapper_is_finalized_when_its_pool_thread_exits()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Disposed_binding_receive_wrapper_is_finalized_when_its_pool_thread_exits)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(() =>
        {
            Exception? failure = null;
            var receiver = new Thread(() =>
            {
                try
                {
                    using var context = Systems.Zlink.Zlink.CreateContext();
                    using var server = context.CreateRouterSocket();
                    using var client = context.CreateRouterSocket();
                    using var poller = Systems.Zlink.Zlink.CreatePoller();
                    var target = RoutingId.From("pool-target");
                    server.SetRoutingId(target);
                    client.SetRoutingId(RoutingId.From("pool-source"));
                    server.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
                    var endpoint = $"inproc://pool-control-{Guid.NewGuid():N}";
                    server.Bind(endpoint);
                    poller.Add(client, PollEventFlags.PollRoute, 1);
                    client.Connect(endpoint);
                    var events = new PollEvent[1];
                    while (client.RoutesSnapshot().Count == 0)
                        Assert.True(poller.Wait(events, TimeSpan.FromSeconds(5)) > 0);
                    using var sent = Message.From("pooled-receive");
                    client.Send(target).Message(sent).Submit();
                    using var received = Received.Create();
                    Assert.True(server.Recv(received));
                    Assert.Equal(
                        "pooled-receive",
                        System.Text.Encoding.UTF8.GetString(
                            received.SinglePartOrThrow().AsReadOnlySpan()
                        )
                    );
                }
                catch (Exception error)
                {
                    failure = error;
                }
            });
            receiver.Start();
            Assert.True(receiver.Join(TimeSpan.FromSeconds(10)));
            Assert.Null(failure);
            return Task.CompletedTask;
        });
        output.WriteLine($"disposed binding receive pool on thread exit finalizers={count}");
        Assert.Equal(1, count);
    }

    [Fact]
    public async Task Raw_request_terminal_closes_wrapper()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Raw_request_terminal_closes_wrapper)
            )
        )
            return;
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(() =>
            new DirectRequestWaitCostTests().Direct_request_only_awaits_for_reply_observation()
        );
        output.WriteLine($"raw request finalizers={count}");
        Assert.Equal(0, count);
    }

    private sealed class FallbackStream(bool reject) : IZLinkStream
    {
        public string SessionId => "fallback";
        public RoutingId? RoutingId => null;
        public string? LocalAddr => null;
        public string? RemoteAddr => null;

        public bool Write(ZLinkMessage payload, SendFlags flags = SendFlags.None) => !reject;

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }
}

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task Durable_activation_metadata_failure_closes_wrappers()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                GetType(),
                nameof(Durable_activation_metadata_failure_closes_wrappers)
            )
        )
            return;
        using var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            )
        );
        await using var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "metadata-target",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(5)
        );
        var source = RoutingId.From("metadata-source");
        var operationId = new MeshOperationId(1, 1);
        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                "channel",
                RoutingId.From("node"),
                1,
                "metadata-target",
                "type",
                "version"
            ),
            source,
            1,
            "source-spot",
            operationId,
            true,
            1,
            checked((ulong)DateTimeOffset.UtcNow.AddMinutes(1).ToUnixTimeMilliseconds())
        );
        using var probe = new MessageFinalizerProbe();
        var count = await probe.CountAsync(async () =>
        {
            for (var index = 0; index < 16; index++)
            {
                var error = await Assert.ThrowsAsync<ArgumentException>(() =>
                    activation
                        .AdmitDurableActivationAsync(
                            operationId,
                            source,
                            "source-spot",
                            new ZLinkServiceWireCodec.RequestSourceFence("owner", 1, source, 1),
                            1,
                            1,
                            1,
                            new ReadOnlyMemory<byte>[] { new byte[] { 1 }, new byte[] { 2 } },
                            new byte[] { 255 },
                            true,
                            CancellationToken.None,
                            operation
                        )
                        .AsTask()
                );
                Assert.Equal("metadata", error.ParamName);
            }
        });
        Assert.Equal(0, count);
    }
}
