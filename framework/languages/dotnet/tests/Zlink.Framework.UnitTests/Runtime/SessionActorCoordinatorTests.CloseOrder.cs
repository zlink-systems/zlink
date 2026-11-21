using System.Diagnostics.Metrics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed partial class SessionActorCoordinatorTests
{
    [Theory]
    [InlineData(0, false)]
    [InlineData(0, true)]
    [InlineData(1, false)]
    [InlineData(1, true)]
    [InlineData(2, false)]
    [InlineData(2, true)]
    public async Task Close_waits_for_prior_Core_admission_in_the_binding_lane(
        int kind,
        bool cancelClose
    )
    {
        for (var iteration = 0; iteration < 40; iteration++)
        {
            var events = new System.Collections.Concurrent.ConcurrentQueue<string>();
            var runtime = CreateRuntime();
            var socket = new AdmissionStreamSocket(events);
            var stream = new ZLinkManagedStream(
                socket,
                RoutingId.From("ordered-session"),
                runtime.Registration.Codecs,
                "tcp"
            );
            var context = new ZLinkSessionContext(
                runtime,
                stream,
                new TestSessionHandlerRegistry(),
                () =>
                {
                    events.Enqueue("close");
                    return ValueTask.CompletedTask;
                },
                static _ => ValueTask.CompletedTask
            );
            var table = new ZLinkSessionActorBindingTable(
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(1)
            );
            var route = SessionBindingRoute("ordered-actor", 1);
            var owner = RoutingId.From("ordered-owner");
            var actor = new ZLinkSessionActor(
                context,
                route.Ref.ActorId,
                stream.RoutingId!.Value,
                "ordered-binding"
            );
            await table.BindAsync(
                ZLinkActorId.FromBoundary(route.Ref.ActorId, "actor"),
                context,
                actor.BindingToken,
                actor,
                5,
                route,
                7,
                owner,
                "owner",
                11
            );
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq | ZlinkStreamHeaderFlags.HasActorSlot,
                new ZlinkStreamRequestSeq(1),
                "",
                ZlinkStreamMetadata.Empty,
                ActorSlot: actor.Slot
            );
            var frame = ActorFrame([1, 2, 3]);
            var entry = (await table.GetBindingAsync(route.Ref.ActorId, actor.BindingToken))!;
            var send = kind switch
            {
                0 => table.SubmitStreamReplyAsync(
                    context,
                    header,
                    _ => socket.SendAsync(stream.RoutingId.Value, Message.From(frame))
                ),
                1 => table
                    .AdmitOutboundAsync(
                        new(
                            route.Ref.ActorId,
                            route.Ref.ObjectGeneration,
                            5,
                            7,
                            stream.RoutingId.Value
                        ),
                        frame
                    )
                    .AsTask(),
                _ => table.DeliverReplyAsync(route.Ref.ActorId, entry, frame).AsTask(),
            };
            await socket.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var request = new ServiceWireCodec.BoundSessionClose54(
                0,
                new(1),
                new(new(route.Ref.ActorId), new(route.Ref.ObjectGeneration)),
                new(
                    new(owner.ToBytes().ToArray()),
                    new(7),
                    new("owner"),
                    new(11),
                    new(stream.RoutingId.Value.ToBytes().ToArray()),
                    new(5)
                )
            );
            using var cancellation = new CancellationTokenSource();
            var close = table
                .CloseCurrentAsync(
                    new ZLinkServiceWireCodec.RequestSourceFence("owner", 11, owner, 7),
                    request,
                    cancellation.Token
                )
                .AsTask();
            if (cancelClose)
                cancellation.Cancel();
            socket.Release.TrySetResult();
            await send.WaitAsync(TimeSpan.FromSeconds(5));
            if (cancelClose)
            {
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => close);
                Assert.Equal(new[] { "admitted" }, events);
            }
            else
            {
                await close.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(new[] { "admitted", "close" }, events);
            }
        }
    }

    [Fact]
    public async Task Pending_submission_does_not_block_another_binding_bind_or_close()
    {
        var failures = new List<string>();
        for (var iteration = 0; iteration < 40; iteration++)
        {
            var runtime = CreateRuntime();
            var owner = RoutingId.From("ordered-owner");
            var events = new System.Collections.Concurrent.ConcurrentQueue<string>();
            var socket = new AdmissionStreamSocket(events);
            var stream = new ZLinkManagedStream(
                socket,
                RoutingId.From("blocked-session"),
                runtime.Registration.Codecs,
                "tcp"
            );
            var contextA = new ZLinkSessionContext(
                runtime,
                stream,
                new TestSessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var contextB = CreateSessionContext(runtime, "independent-session-b");
            var contextC = CreateSessionContext(runtime, "independent-session-c");
            var table = new ZLinkSessionActorBindingTable(
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(1)
            );
            var routeA = SessionBindingRoute("blocked-actor-a", 1);
            var routeB = SessionBindingRoute("independent-actor-b", 1);
            var routeC = SessionBindingRoute("independent-actor-c", 1);

            async Task<ZLinkSessionActor> BindAsync(
                ZLinkSessionBindingRoute route,
                ZLinkSessionContext context,
                string token
            )
            {
                var actor = new ZLinkSessionActor(
                    context,
                    route.Ref.ActorId,
                    context.RoutingId!.Value,
                    token
                );
                await table.BindAsync(
                    ZLinkActorId.FromBoundary(route.Ref.ActorId, "actor"),
                    context,
                    token,
                    actor,
                    5,
                    route,
                    7,
                    owner,
                    "owner",
                    11
                );
                return actor;
            }

            await BindAsync(routeA, contextA, "binding-a");
            await BindAsync(routeB, contextB, "binding-b");
            var pending = table
                .AdmitOutboundAsync(
                    new(
                        routeA.Ref.ActorId,
                        routeA.Ref.ObjectGeneration,
                        5,
                        7,
                        stream.RoutingId!.Value
                    ),
                    ActorFrame([1, 2, 3])
                )
                .AsTask();
            await socket.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

            var bindC = BindAsync(routeC, contextC, "binding-c");
            var closeB = table
                .CloseCurrentAsync(
                    new ZLinkServiceWireCodec.RequestSourceFence("owner", 11, owner, 7),
                    new ServiceWireCodec.BoundSessionClose54(
                        0,
                        new(1),
                        new(new(routeB.Ref.ActorId), new(routeB.Ref.ObjectGeneration)),
                        new(
                            new(owner.ToBytes().ToArray()),
                            new(7),
                            new("owner"),
                            new(11),
                            new(contextB.RoutingId!.Value.ToBytes().ToArray()),
                            new(5)
                        )
                    ),
                    CancellationToken.None
                )
                .AsTask();
            try
            {
                await Task.WhenAll(bindC, closeB).WaitAsync(TimeSpan.FromMilliseconds(500));
            }
            catch (TimeoutException)
            {
                failures.Add($"iteration={iteration}: another binding did not bind and close");
            }
            catch (Exception error)
            {
                failures.Add($"iteration={iteration}: {error.GetType().Name}: {error.Message}");
            }
            finally
            {
                socket.Release.TrySetResult();
                await pending.WaitAsync(TimeSpan.FromSeconds(5));
                await Task.WhenAll(bindC, closeB).WaitAsync(TimeSpan.FromSeconds(5));
            }
        }

        Assert.Empty(failures);
    }

    [Fact]
    public async Task Expired_close_deadline_releases_host_state_lane_and_skips_close()
    {
        var failures = new List<string>();
        for (var iteration = 0; iteration < 40; iteration++)
        {
            var runtime = CreateRuntime();
            var owner = RoutingId.From("ordered-owner");
            var events = new System.Collections.Concurrent.ConcurrentQueue<string>();
            var socket = new AdmissionStreamSocket(events);
            var stream = new ZLinkManagedStream(
                socket,
                RoutingId.From("deadline-session"),
                runtime.Registration.Codecs,
                "tcp"
            );
            var closeCount = 0;
            var contextA = new ZLinkSessionContext(
                runtime,
                stream,
                new TestSessionHandlerRegistry(),
                () =>
                {
                    Interlocked.Increment(ref closeCount);
                    return ValueTask.CompletedTask;
                },
                static _ => ValueTask.CompletedTask
            );
            var contextB = CreateSessionContext(runtime, "deadline-independent-session");
            var table = new ZLinkSessionActorBindingTable(
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(1)
            );
            var routeA = SessionBindingRoute("deadline-actor-a", 1);
            var routeB = SessionBindingRoute("deadline-actor-b", 1);
            var actorA = new ZLinkSessionActor(
                contextA,
                routeA.Ref.ActorId,
                stream.RoutingId!.Value,
                "deadline-binding-a"
            );
            var actorB = new ZLinkSessionActor(
                contextB,
                routeB.Ref.ActorId,
                contextB.RoutingId!.Value,
                "deadline-binding-b"
            );

            async Task BindAsync(
                ZLinkSessionBindingRoute route,
                ZLinkSessionActor actor,
                ZLinkSessionContext context
            ) =>
                await table.BindAsync(
                    ZLinkActorId.FromBoundary(route.Ref.ActorId, "actor"),
                    context,
                    actor.BindingToken,
                    actor,
                    5,
                    route,
                    7,
                    owner,
                    "owner",
                    11
                );

            await BindAsync(routeA, actorA, contextA);
            var pending = table
                .AdmitOutboundAsync(
                    new(
                        routeA.Ref.ActorId,
                        routeA.Ref.ObjectGeneration,
                        5,
                        7,
                        stream.RoutingId!.Value
                    ),
                    ActorFrame([4, 5, 6])
                )
                .AsTask();
            await socket.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(40));
            var expiredClose = table
                .CloseCurrentAsync(
                    new ZLinkServiceWireCodec.RequestSourceFence("owner", 11, owner, 7),
                    new ServiceWireCodec.BoundSessionClose54(
                        0,
                        new(2),
                        new(new(routeA.Ref.ActorId), new(routeA.Ref.ObjectGeneration)),
                        new(
                            new(owner.ToBytes().ToArray()),
                            new(7),
                            new("owner"),
                            new(11),
                            new(stream.RoutingId.Value.ToBytes().ToArray()),
                            new(5)
                        )
                    ),
                    deadline.Token
                )
                .AsTask();
            try
            {
                await expiredClose.WaitAsync(TimeSpan.FromMilliseconds(500));
                failures.Add($"iteration={iteration}: close completed before its deadline");
            }
            catch (OperationCanceledException) when (deadline.IsCancellationRequested) { }
            catch (TimeoutException)
            {
                failures.Add($"iteration={iteration}: close remained blocked past its deadline");
            }
            catch (Exception error)
            {
                failures.Add($"iteration={iteration}: {error.GetType().Name}: {error.Message}");
            }

            var bindB = BindAsync(routeB, actorB, contextB);
            var closeB = table
                .CloseCurrentAsync(
                    new ZLinkServiceWireCodec.RequestSourceFence("owner", 11, owner, 7),
                    new ServiceWireCodec.BoundSessionClose54(
                        0,
                        new(3),
                        new(new(routeB.Ref.ActorId), new(routeB.Ref.ObjectGeneration)),
                        new(
                            new(owner.ToBytes().ToArray()),
                            new(7),
                            new("owner"),
                            new(11),
                            new(contextB.RoutingId!.Value.ToBytes().ToArray()),
                            new(5)
                        )
                    ),
                    CancellationToken.None
                )
                .AsTask();
            try
            {
                await Task.WhenAll(bindB, closeB).WaitAsync(TimeSpan.FromMilliseconds(500));
            }
            catch (TimeoutException)
            {
                failures.Add(
                    $"iteration={iteration}: binding lane did not progress after deadline"
                );
            }
            catch (Exception error)
            {
                failures.Add($"iteration={iteration}: {error.GetType().Name}: {error.Message}");
            }
            finally
            {
                socket.Release.TrySetResult();
                await pending.WaitAsync(TimeSpan.FromSeconds(5));
                await Task.WhenAll(bindB, closeB).WaitAsync(TimeSpan.FromSeconds(5));
                if (!expiredClose.IsCompleted)
                {
                    try
                    {
                        await expiredClose.WaitAsync(TimeSpan.FromSeconds(5));
                    }
                    catch (OperationCanceledException) when (deadline.IsCancellationRequested) { }
                }
                var afterDeadline = await table
                    .AdmitOutboundAsync(
                        new(
                            routeA.Ref.ActorId,
                            routeA.Ref.ObjectGeneration,
                            5,
                            7,
                            stream.RoutingId.Value
                        ),
                        ActorFrame([7, 8, 9])
                    )
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(ZLinkSessionOutboundDelivery.Delivered, afterDeadline.Delivery);
            }

            Assert.Equal(0, Volatile.Read(ref closeCount));
        }

        Assert.Empty(failures);
    }

    private sealed class AdmissionStreamSocket(
        System.Collections.Concurrent.ConcurrentQueue<string> events
    ) : IZLinkBackendStreamSocket
    {
        internal TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Bind(string endpoint) { }

        public void SetTlsServer(string certPath, string keyPath, bool requireClientCert) { }

        public bool RecvPacket(
            out ZLinkBackendStreamReceive? received,
            RecvFlags flags = RecvFlags.None
        )
        {
            received = null;
            return false;
        }

        public async Task SendAsync(
            RoutingId routingId,
            Message payload,
            CancellationToken cancellationToken = default
        )
        {
            using (payload)
            {
                Started.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
                events.Enqueue("admitted");
            }
        }

        public void DisconnectPeer(RoutingId routingId) { }

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags
        ) => throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
