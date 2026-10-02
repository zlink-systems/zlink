using System.Diagnostics;
using System.Reflection;
using Systems.Zlink;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotPeerConnectorTests
{
    [Fact]
    public void Auto_Router_Connect_Preserves_Busy_And_Rolls_Back_Claim()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, BusyOnceSpotNode>();
        var proxy = (BusyOnceSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());

        var failure = Assert.Throws<ZlinkConnectException>(() =>
            connector.ConnectPeerAuto(RoutingId.From("peer"), "tcp://peer:1", "none")
        );
        Assert.Same(proxy.ConnectFailure, failure);
        Assert.Equal(1, proxy.ConnectAttempts);

        // A separate explicit request must reach the backend after claim rollback.
        Assert.True(connector.ConnectPeerAuto(RoutingId.From("peer"), "tcp://peer:1", "none"));
        Assert.Equal(2, proxy.ConnectAttempts);
    }

    [Fact]
    public void Auto_Router_Replaces_A_Different_Rid_At_The_Same_Endpoint()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, ReplacementSpotNode>();
        var proxy = (ReplacementSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());
        var oldRid = RoutingId.From("old-peer");
        var newRid = RoutingId.From("new-peer");

        Assert.True(connector.ConnectPeerAuto(oldRid, "tcp://peer:1", "none"));
        Assert.True(connector.ConnectPeerAuto(newRid, "tcp://peer:1", "none"));

        Assert.Equal([oldRid, newRid], proxy.ConnectedRids);
        Assert.Equal(["tcp://peer:1"], proxy.DisconnectedEndpoints);

        // Removing the stale target must not tear down the replacement
        // connection that now owns this endpoint.
        Assert.True(connector.DisconnectPeerAuto(oldRid, "tcp://peer:1"));
        Assert.Single(proxy.DisconnectedEndpoints);
        Assert.Equal((oldRid, "tcp://peer:1", 1UL), proxy.AdmissionCleanup);

        Assert.True(connector.DisconnectPeerAuto(newRid, "tcp://peer:1"));
        Assert.Single(proxy.DisconnectedEndpoints);
        Assert.Equal((newRid, "tcp://peer:1", 1UL), proxy.AdmissionCleanup);
    }

    [Fact]
    public void Auto_NonInitiator_Delegates_Admission_Pending_Cleanup()
    {
        var node = DispatchProxy.Create<IZLinkBackendSpotNode, CleanupSpotNode>();
        var proxy = (CleanupSpotNode)(object)node;
        var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());
        var peerRid = RoutingId.From("peer");

        Assert.True(
            connector.DisconnectPeerBeforeAdmission(peerRid, "tcp://peer:1", lifecycleGeneration: 7)
        );
        Assert.Equal((peerRid, "tcp://peer:1", 7UL), proxy.Cleanup);
    }

    [Fact]
    public void Auto_Router_Logs_None_For_An_Absent_Peer_Rid()
    {
        if (Environment.GetEnvironmentVariable("ZLINK_TEST_FRAMEWORK_DEBUG_LOG_PROBE") == "enabled")
        {
            var originalError = Console.Error;
            using var capturedError = new StringWriter();
            try
            {
                Console.SetError(capturedError);
                var node = DispatchProxy.Create<IZLinkBackendSpotNode, BusyOnceSpotNode>();
                var proxy = (BusyOnceSpotNode)(object)node;
                var connector = new ZLinkSpotPeerConnector(node, new ZLinkSpotPeerConnectionSet());

                var failure = Assert.Throws<ZlinkConnectException>(() =>
                    connector.ConnectPeerAuto(null, "tcp://peer:1", "none")
                );
                Assert.Same(proxy.ConnectFailure, failure);
                Assert.Equal(1, proxy.ConnectAttempts);
                Assert.True(connector.ConnectPeerAuto(null, "tcp://peer:1", "none"));
                Assert.Equal(2, proxy.ConnectAttempts);
            }
            finally
            {
                Console.SetError(originalError);
            }

            Assert.Contains("spot_peer_claim peer=<none>", capturedError.ToString());
            return;
        }

        RunIsolatedTraceProbe(nameof(Auto_Router_Logs_None_For_An_Absent_Peer_Rid), "enabled");
    }

    [Fact]
    public void Spot_Discovery_Handler_Does_Not_Evaluate_Holes_When_Disabled()
    {
        if (
            Environment.GetEnvironmentVariable("ZLINK_TEST_FRAMEWORK_DEBUG_LOG_PROBE") == "disabled"
        )
        {
            Assert.False(ZLinkFrameworkDebugLog.SpotDiscoveryEnabled);
            var evaluations = 0;

            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"interpolation={Interlocked.Increment(ref evaluations)}"
            );

            Assert.Equal(0, evaluations);
            return;
        }

        RunIsolatedTraceProbe(
            nameof(Spot_Discovery_Handler_Does_Not_Evaluate_Holes_When_Disabled),
            "disabled"
        );
    }

    private static void RunIsolatedTraceProbe(string testName, string mode)
    {
        var projectPath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                "..",
                "..",
                "..",
                "Zlink.Framework.UnitTests.csproj"
            )
        );
        var startInfo = new ProcessStartInfo("dotnet")
        {
            WorkingDirectory = Path.GetDirectoryName(projectPath)!,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        startInfo.ArgumentList.Add("test");
        startInfo.ArgumentList.Add(projectPath);
        startInfo.ArgumentList.Add("--no-build");
        startInfo.ArgumentList.Add("--no-restore");
        startInfo.ArgumentList.Add("--framework");
        startInfo.ArgumentList.Add(new DirectoryInfo(AppContext.BaseDirectory).Name);
        startInfo.ArgumentList.Add("--filter");
        startInfo.ArgumentList.Add($"FullyQualifiedName~{testName}");
        startInfo.ArgumentList.Add("--verbosity");
        startInfo.ArgumentList.Add("quiet");
        startInfo.Environment["ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY"] =
            mode == "enabled" ? "1" : "0";
        startInfo.Environment["ZLINK_TEST_FRAMEWORK_DEBUG_LOG_PROBE"] = mode;

        using var process = Process.Start(startInfo);
        Assert.NotNull(process);
        var output = process!.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        process.WaitForExit();
        Task.WaitAll(output, error);

        Assert.True(
            process.ExitCode == 0,
            $"Isolated trace probe '{testName}' failed.{Environment.NewLine}{output.Result}{error.Result}"
        );
    }

    private class BusyOnceSpotNode : DispatchProxy
    {
        internal int ConnectAttempts { get; private set; }

        internal ZlinkConnectException ConnectFailure { get; } =
            new(ZlinkConnectException.ErrorCode.Busy);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name != nameof(IZLinkBackendSpotNode.ConnectPeer))
                throw new NotSupportedException(targetMethod.Name);

            ConnectAttempts++;
            if (ConnectAttempts == 1)
                throw ConnectFailure;

            return null;
        }
    }

    private class CleanupSpotNode : DispatchProxy
    {
        internal (RoutingId Rid, string Endpoint, ulong Generation)? Cleanup { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            if (targetMethod.Name != nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmission))
                throw new NotSupportedException(targetMethod.Name);

            Cleanup = ((RoutingId)args![0]!, (string)args[1]!, (ulong)args[2]!);
            return true;
        }
    }

    private class ReplacementSpotNode : DispatchProxy
    {
        internal List<RoutingId> ConnectedRids { get; } = [];

        internal List<string> DisconnectedEndpoints { get; } = [];

        internal (RoutingId Rid, string Endpoint, ulong Generation)? AdmissionCleanup
        {
            get;
            private set;
        }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            switch (targetMethod.Name)
            {
                case nameof(IZLinkBackendSpotNode.ConnectPeer) when args is { Length: 3 }:
                    ConnectedRids.Add((RoutingId)args[0]!);
                    return null;
                case nameof(IZLinkBackendSpotNode.DisconnectPeer):
                    DisconnectedEndpoints.Add((string)args![0]!);
                    return null;
                case nameof(IZLinkBackendSpotNode.MeshPeers):
                    return ConnectedRids
                        .Select(
                            (rid, index) =>
                                new MeshNodePeer(
                                    ConnectionIntentId: (ulong)index + 1,
                                    Source: MeshPeerSource.Discovery,
                                    State: MeshPeerState.Connecting,
                                    RoutingId: rid,
                                    LifecycleGeneration: 1,
                                    DescriptorRevision: 1,
                                    Endpoint: "tcp://peer:1",
                                    ChannelCount: 0,
                                    LastError: 0,
                                    LastChangedMs: 0
                                )
                        )
                        .ToArray();
                case nameof(IZLinkBackendSpotNode.DisconnectPeerBeforeAdmission):
                    AdmissionCleanup = ((RoutingId)args![0]!, (string)args[1]!, (ulong)args[2]!);
                    return true;
                default:
                    throw new NotSupportedException(targetMethod.Name);
            }
        }
    }
}
