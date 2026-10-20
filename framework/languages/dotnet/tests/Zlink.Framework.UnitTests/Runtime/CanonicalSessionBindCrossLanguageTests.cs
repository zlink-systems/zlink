using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class BoundSessionReplacementLifecycleTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Canonical_bind_crosses_Node_and_Dotnet_owners(bool nodeSessionOwner)
    {
        var root = Zlink.Framework.Tests.Common.FrameworkTestEnvironment.GetRepoRoot();
        var log = Path.Combine(
            root,
            "logs",
            $"cross-bind-{nodeSessionOwner}-{DateTime.UtcNow:HHmmssfff}"
        );
        Directory.CreateDirectory(log);
        using var flow = new TestHostMessageFlowListener(Path.Combine(log, "dotnet.flow"));
        await using var node = await NodeBindHost.StartAsync(root, log);
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseSourceLeave.TrySetResult();
        PeerDescriptorFilter filter = null!;
        await using var host = await RelocationBehaviorHost.StartAsync(
            "bind",
            trace,
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            false,
            pollingInterval: TimeSpan.FromDays(1),
            meshResolverWrapper: inner => filter = new PeerDescriptorFilter(inner)
        );
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );
        var actor = Assert
            .IsType<ZLinkActorCreateResult.Created>(
                await host
                    .Services.GetRequiredService<IZLinkActorManager>()
                    .GetOrCreate("dotnet-bind-actor", RelocationBehaviorHost.ActorType)
                    .InMesh(RelocationBehaviorHost.MeshName)
                    .Request(new BehaviorCreate(7))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async()
            )
            .Actor;
        var repository = host.Services.GetRequiredService<IZLinkLocationRepository>();
        await repository.ClaimLiveOwnerAsync(node.OwnerId, TimeSpan.FromMinutes(5));
        var nodeActor = await AuthorityLocationTestFixture.PublishActorAsync(
            repository,
            InMemoryLocationStoreTests.Actor(node.OwnerId, "cross-bind-actor") with
            {
                MeshName = RelocationBehaviorHost.MeshName,
                ActorType = "actor",
                ActorRef = new ActorRef(
                    "cross-bind-actor",
                    node.ObjectGeneration,
                    RelocationBehaviorHost.MeshName,
                    RoutingId.From("cross-bind-node")
                ),
                OwnerNodeRid = RoutingId.From("cross-bind-node"),
                OwnerNodeGeneration = node.Generation,
                AuthorityOwnerGeneration = node.Authority,
                LeaseGeneration = 1,
            },
            configureDescriptor: descriptor =>
                descriptor with
                {
                    Endpoint = node.Endpoint,
                    SecurityIdentity = ZLinkServiceSecurityIdentity.Plaintext,
                }
        );
        Assert.NotNull(nodeActor);
        node.ObjectGeneration = nodeActor.ObjectGeneration;
        node.Authority = nodeActor.AuthorityOwnerGeneration;
        await node.CommandAsync(
            new
            {
                kind = "actor",
                generation = node.ObjectGeneration.ToString(),
                authority = node.Authority.ToString(),
            }
        );
        var local = host.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName);
        var owner = local.LocalSessionOwner!.Value;
        var rid = Encoding.UTF8.GetString(local.Node.RoutingId.ToBytes());
        await node.CommandAsync(
            new
            {
                kind = "configure",
                rid,
                generation = owner.NodeGeneration.ToString(),
                ownerId = owner.OwnerId,
                lease = owner.LeaseGeneration.ToString(),
                endpoint = local.Node.MeshStatus().LocalEndpoint,
            }
        );
        local.Node.ConnectPeer(RoutingId.From("cross-bind-node"), node.Endpoint);
        await WaitUntilAsync(() => local.Node.MeshStatus().AdmittedPeerCount == 1);

        if (nodeSessionOwner)
        {
            Assert.True(
                ((IZLinkBackendLocalActorAuthorityReader)local.Node).TryGetLocalActorAuthority(
                    actor.ToBackend(),
                    out var actorAuthority,
                    out var actorLease
                )
            );
            object Bind(string session) =>
                new
                {
                    kind = "bind",
                    rid,
                    actorId = actor.ActorId,
                    objectGeneration = actor.ObjectGeneration.ToString(),
                    nodeGeneration = owner.NodeGeneration.ToString(),
                    authority = actorAuthority.ToString(),
                    lease = actorLease.ToString(),
                    session,
                };
            filter.Visible = false;
            var unavailable = await node.CommandAsync(Bind("node-missing-descriptor"));
            Assert.Equal("terminal", unavailable.GetProperty("kind").GetString());
            Assert.Equal(
                105,
                unavailable.GetProperty("result").GetProperty("terminalResult").GetInt32()
            );
            Assert.Equal(
                13,
                unavailable.GetProperty("result").GetProperty("failureCode").GetInt32()
            );
            filter.Visible = true;
            var first = await node.CommandAsync(Bind("node-session-first"));
            Assert.Equal(0, first.GetProperty("result").GetProperty("terminalResult").GetInt32());
            Assert.True(
                host.Runtime.TryGetActorBoundSessionForOutbound(actor.ActorId, out var original)
            );
            Assert.Equal(RoutingId.From("node-session-first"), original.SessionRid);
            var second = await node.CommandAsync(Bind("node-session-second"));
            Assert.Equal(0, second.GetProperty("result").GetProperty("terminalResult").GetInt32());
            Assert.True(
                host.Runtime.TryGetActorBoundSessionForOutbound(actor.ActorId, out var replacement)
            );
            Assert.Equal(RoutingId.From("node-session-second"), replacement.SessionRid);
            foreach (
                var (session, generation, success) in new[]
                {
                    ("node-session-second", replacement.BindingGeneration, true),
                    ("node-session-first", original.BindingGeneration, false),
                    ("node-session-conflict", replacement.BindingGeneration, false),
                }
            )
            {
                var reply = await node.CommandAsync(
                    new
                    {
                        kind = "register",
                        rid,
                        actorId = actor.ActorId,
                        objectGeneration = actor.ObjectGeneration.ToString(),
                        nodeGeneration = owner.NodeGeneration.ToString(),
                        authority = actorAuthority.ToString(),
                        lease = actorLease.ToString(),
                        session,
                        bindingGeneration = generation.ToString(),
                    }
                );
                var terminal = reply.GetProperty("result");
                Assert.Equal(success ? 0 : 105, terminal.GetProperty("terminalResult").GetInt32());
                if (success)
                {
                    var tail = terminal.GetProperty("tail");
                    Assert.Equal(
                        generation.ToString(),
                        tail.GetProperty("bindingGeneration").GetString()
                    );
                    Assert.Equal(
                        actorAuthority.ToString(),
                        tail.GetProperty("authorityOwnerGeneration").GetString()
                    );
                }
                Assert.True(
                    host.Runtime.TryGetActorBoundSessionForOutbound(actor.ActorId, out var retained)
                );
                Assert.Equal(replacement, retained);
            }
            var retired = await node.CommandAsync(
                new
                {
                    kind = "tombstone",
                    rid,
                    actorId = actor.ActorId,
                    objectGeneration = actor.ObjectGeneration.ToString(),
                    nodeGeneration = owner.NodeGeneration.ToString(),
                    authority = actorAuthority.ToString(),
                    lease = actorLease.ToString(),
                    session = "node-session-first",
                    bindingGeneration = original.BindingGeneration.ToString(),
                }
            );
            Assert.Equal(0, retired.GetProperty("result").GetProperty("terminalResult").GetInt32());
            Assert.True(
                host.Runtime.TryGetActorBoundSessionForOutbound(actor.ActorId, out var surviving)
            );
            Assert.Equal(replacement.BindingGeneration, surviving.BindingGeneration);
            Assert.Equal(replacement.SessionRid, surviving.SessionRid);
        }
        else
        {
            await using var first = await ReplacementFixture.CreateAsync(
                ReplacementCallbackBehavior.Success,
                sharedRuntime: host.Runtime,
                sessionRid: RoutingId.From("dotnet-first")
            );
            var target = new ActorRef(
                "cross-bind-actor",
                node.ObjectGeneration,
                RelocationBehaviorHost.MeshName,
                RoutingId.From("cross-bind-node")
            );
            await node.CommandAsync(new { kind = "descriptor", visible = false });
            var unavailable = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await first.Lifetime.Context.ActorCoordinator.BindActorAsync(
                    first.Lifetime.Context,
                    target,
                    CancellationToken.None
                )
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, unavailable.Kind);
            await node.CommandAsync(new { kind = "descriptor", visible = true });
            Assert.NotNull(
                await first.Lifetime.Context.ActorCoordinator.BindActorAsync(
                    first.Lifetime.Context,
                    target,
                    CancellationToken.None
                )
            );
            var current = await node.CommandAsync(new { kind = "binding" });
            Assert.Equal(
                "dotnet-first",
                current.GetProperty("binding").GetProperty("sessionRid").GetString()
            );
            await using var second = await ReplacementFixture.CreateAsync(
                ReplacementCallbackBehavior.Success,
                sharedRuntime: host.Runtime,
                sessionRid: RoutingId.From("dotnet-second")
            );
            Assert.NotNull(
                await second.Lifetime.Context.ActorCoordinator.BindActorAsync(
                    second.Lifetime.Context,
                    target,
                    CancellationToken.None
                )
            );
            current = await node.CommandAsync(new { kind = "binding" });
            Assert.Equal(
                "dotnet-second",
                current.GetProperty("binding").GetProperty("sessionRid").GetString()
            );
            await first.Lifetime.CallbackTerminal.Task.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal(1, first.Lifetime.CallbackCount);
        }
    }

    private sealed class PeerDescriptorFilter(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        internal bool Visible = true;

        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) =>
            (await inner.ListLiveMeshNodesAsync(meshName, cancellationToken))
                .Where(item => Visible || item.Rid != RoutingId.From("cross-bind-node"))
                .ToArray();
    }

    private sealed class NodeBindHost(Process process, string endpoint, string log)
        : IAsyncDisposable
    {
        internal string Endpoint { get; private set; } = endpoint;
        internal ulong Generation;
        internal ulong ObjectGeneration;
        internal ulong Authority;
        internal string OwnerId = "";
        private readonly Task<string> _stderr = process.StandardError.ReadToEndAsync();

        internal static async Task<NodeBindHost> StartAsync(string root, string log)
        {
            var endpoint = "tcp://127.0.0.1:0";
            var start = new ProcessStartInfo("node")
            {
                WorkingDirectory = Path.Combine(root, "framework/languages/node"),
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            start.ArgumentList.Add("cross-language/session_bind_host.js");
            start.ArgumentList.Add(endpoint);
            start.ArgumentList.Add(RelocationBehaviorHost.MeshName);
            var process = Process.Start(start)!;
            var result = new NodeBindHost(process, endpoint, log);
            try
            {
                var ready = await result.ReadAsync();
                Assert.Equal("ready", ready.GetProperty("kind").GetString());
                result.Endpoint = ready.GetProperty("endpoint").GetString()!;
                result.Generation = ulong.Parse(ready.GetProperty("generation").GetString()!);
                result.ObjectGeneration = ulong.Parse(
                    ready.GetProperty("actor").GetProperty("generation").GetString()!
                );
                result.Authority = ulong.Parse(ready.GetProperty("authority").GetString()!);
                result.OwnerId = ready.GetProperty("ownerId").GetString()!;
                return result;
            }
            catch
            {
                await result.DisposeAsync();
                throw;
            }
        }

        internal async Task<JsonElement> CommandAsync(object command)
        {
            await process.StandardInput.WriteLineAsync(JsonSerializer.Serialize(command));
            await process.StandardInput.FlushAsync();
            return await ReadAsync();
        }

        private async Task<JsonElement> ReadAsync()
        {
            while (true)
            {
                var line = await process
                    .StandardOutput.ReadLineAsync()
                    .WaitAsync(TimeSpan.FromSeconds(10));
                Assert.NotNull(line);
                await File.AppendAllTextAsync(
                    Path.Combine(log, "node.stdout.log"),
                    line + Environment.NewLine
                );
                if (!line.StartsWith('{'))
                    continue;
                using var json = JsonDocument.Parse(line);
                return json.RootElement.Clone();
            }
        }

        public async ValueTask DisposeAsync()
        {
            process.StandardInput.Close();
            try
            {
                await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            }
            catch (TimeoutException)
            {
                process.Kill(true);
                await process.WaitForExitAsync();
            }
            await File.WriteAllTextAsync(Path.Combine(log, "node.stderr.log"), await _stderr);
            process.Dispose();
        }
    }
}
