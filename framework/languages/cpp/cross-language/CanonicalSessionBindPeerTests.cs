using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed partial class BoundSessionReplacementLifecycleTests
{
    [Theory]
    [InlineData("java")]
    [InlineData("cpp")]
    public async Task Canonical_bind_crosses_JavaCpp_and_Dotnet_owners(string peerLanguage)
    {
        var root = Zlink.Framework.Tests.Common.FrameworkTestEnvironment.GetRepoRoot();
        var log = Path.Combine(
            root,
            "logs",
            $"cross-bind-{peerLanguage}-{DateTime.UtcNow:HHmmssfff}"
        );
        Directory.CreateDirectory(log);
        using var flow = new TestHostMessageFlowListener(Path.Combine(log, "dotnet.flow"));
        await using var node = await ForeignBindHost.StartAsync(root, log, peerLanguage);
        var trace = new RelocationBehaviorTrace();
        trace.ReleaseSourceLeave.TrySetResult();
        J2140PeerDescriptorFilter filter = null!;
        await using var host = await RelocationBehaviorHost.StartAsync(
            "bind",
            trace,
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            false,
            pollingInterval: TimeSpan.FromDays(1),
            meshResolverWrapper: inner => filter = new J2140PeerDescriptorFilter(inner)
        );
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );
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

    private sealed class J2140PeerDescriptorFilter(IZLinkMeshNodeLocationResolver inner)
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

    private sealed class ForeignBindHost(Process process, string endpoint, string log)
        : IAsyncDisposable
    {
        internal string Endpoint { get; private set; } = endpoint;
        internal ulong Generation;
        internal ulong ObjectGeneration;
        internal ulong Authority;
        internal string OwnerId = "";
        private readonly Task<string> _stderr = process.StandardError.ReadToEndAsync();

        internal static async Task<ForeignBindHost> StartAsync(
            string root,
            string log,
            string peerLanguage
        )
        {
            var endpoint = "tcp://127.0.0.1:0";
            var start = new ProcessStartInfo("bash")
            {
                WorkingDirectory = root,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            start.ArgumentList.Add(
                Path.Combine(
                    root,
                    "framework/languages/cpp/cross-language/launch_session_bind_peer.sh"
                )
            );
            start.ArgumentList.Add(peerLanguage);
            start.ArgumentList.Add(endpoint);
            start.ArgumentList.Add(RelocationBehaviorHost.MeshName);
            var process = Process.Start(start)!;
            var result = new ForeignBindHost(process, endpoint, log);
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
