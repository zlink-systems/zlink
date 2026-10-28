using System.Reflection;
using System.Text.RegularExpressions;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.HttpClient;

namespace Zlink.Framework.ContractTests.Coverage;

public sealed class SourceLayoutContracts
{
    // 이 목록은 private 접근 시험을 공개 동작 관찰로 옮길 때 줄어들기만 해야 합니다.
    private static readonly string[] LegacyNonPublicReflectionFiles =
    [
        "Zlink.Framework.UnitTests/Configuration/Registration/Monitoring.cs",
        "Zlink.Framework.UnitTests/Runtime/ClientServerClientAckTests.cs",
        "Zlink.Framework.UnitTests/Runtime/ClientServerLivenessLaneTests.cs",
        "Zlink.Framework.UnitTests/Runtime/DescriptorOwnerLeaseSelectionTests.cs",
        "Zlink.Framework.UnitTests/Runtime/DirectRequestWaitCostTests.cs",
        "Zlink.Framework.UnitTests/Runtime/DurableSenderRuntimeTests.cs",
        "Zlink.Framework.UnitTests/Runtime/EntrySpotActorDispatchTests.cs",
        "Zlink.Framework.UnitTests/Runtime/EnvelopeHeaderCacheHotPathTests.cs",
        "Zlink.Framework.UnitTests/Runtime/ErrorReplyWireConvergenceTests.cs",
        "Zlink.Framework.UnitTests/Runtime/LogicalMulticastSubmitTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshChannelSelectionHotPathTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshLivenessDisconnectContractTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshLivenessPhysicalCloseTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshMailboxReadinessTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshMonitorHotPathTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshNodeHelloSubmissionTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshNodeLifecycleStateTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs",
        "Zlink.Framework.UnitTests/Runtime/MeshPermitHandoffTests.cs",
        "Zlink.Framework.UnitTests/Runtime/RawPortCloseFailureTests.cs",
        "Zlink.Framework.UnitTests/Runtime/RelocationBehaviorConformanceTests.cs",
        "Zlink.Framework.UnitTests/Runtime/RelocationSourceRestoreTests.cs",
        "Zlink.Framework.UnitTests/Runtime/RelocationTargetShutdownTests.cs",
        "Zlink.Framework.UnitTests/Runtime/SameNodeActorJoinTests.cs",
        "Zlink.Framework.UnitTests/Runtime/SerialExecutorTests.cs",
        "Zlink.Framework.UnitTests/Runtime/ServiceRuntimeFoundationTests.cs",
        "Zlink.Framework.UnitTests/Runtime/ServiceWireSessionRelocationCodecTests.cs",
        "Zlink.Framework.UnitTests/Runtime/SessionActorCoordinatorTests.cs",
        "Zlink.Framework.UnitTests/Runtime/SessionFailureClassificationTests.cs",
        "Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs",
        "Zlink.Framework.UnitTests/Runtime/SubmitAdmissionRuntimeTests.cs",
    ];

    private static readonly string[] RemovedRuntimeMethods =
    [
        "RecoverCanonicalRemoteJoinCompletionAsync",
        "RunDeferredJoinCompletionRecoveryAsync",
        "RecoverPublishedRelocationsAsync",
        "TryBeginRelocationBarrier",
        "RunOnSharedGate",
        "EncodeRelocationAck",
        "TryDecodeRelocationAck",
        "EncodeRelocationComplete",
        "TryDecodeRelocationComplete",
        "EncodeRelocationReserved",
        "TryDecodeRelocationReserved",
        "EncodeRelocationSeal",
        "TryDecodeRelocationSeal",
        "EncodeSessionRelocationRouted",
        "TryDecodeSessionRelocationRouted",
    ];

    private static readonly string[] RemovedRuntimeTypes =
    [
        "SessionRelocationRoutedRecord",
        "SessionRelocationRouteResult",
    ];

    private static readonly string[] ServerContractAreas =
    [
        "Actors",
        "Channels",
        "Configuration",
        "Dispatch",
        "Handlers",
        "Locations",
        "Messaging",
        "Spots",
        "Streams",
        "Timers",
        "Workers",
    ];

    [Fact]
    public void Server_application_contract_source_is_owned_by_the_server_project()
    {
        var root = FindRepositoryRoot();
        var serverContracts = Path.Combine(
            root,
            "framework/languages/dotnet/src/Zlink.Framework/Contracts"
        );
        var sharedContracts = Path.Combine(
            root,
            "framework/languages/dotnet/src/Zlink.Framework.Contracts"
        );

        foreach (var area in ServerContractAreas)
        {
            Assert.True(
                Directory.Exists(Path.Combine(serverContracts, area)),
                $"The Zlink.Framework project must own the {area} contract source."
            );
            Assert.False(
                Directory.Exists(Path.Combine(sharedContracts, area))
                    && Directory
                        .EnumerateFiles(
                            Path.Combine(sharedContracts, area),
                            "*.cs",
                            SearchOption.AllDirectories
                        )
                        .Any(),
                $"The shared contract project must not own the {area} server contract source."
            );
        }

        var unexpectedSharedSources = Directory
            .EnumerateFiles(sharedContracts, "*.cs", SearchOption.AllDirectories)
            .Where(path => !IsBuildOutput(path))
            .Select(path => Path.GetRelativePath(sharedContracts, path).Replace('\\', '/'))
            .Where(path =>
                path != "GlobalUsings.cs"
                && !path.StartsWith("Codecs/", StringComparison.Ordinal)
                && !path.StartsWith("Errors/", StringComparison.Ordinal)
                && !path.StartsWith("Properties/", StringComparison.Ordinal)
            )
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(unexpectedSharedSources);
    }

    [Fact]
    public void Tests_do_not_use_nonpublic_member_reflection()
    {
        var tests = Path.Combine(FindRepositoryRoot(), "framework/languages/dotnet/tests");
        var forbidden = new Regex(
            string.Concat("BindingFlags\\s*\\.\\s*", "NonPublic"),
            RegexOptions.CultureInvariant
        );
        var matches = Directory
            .EnumerateFiles(tests, "*.cs", SearchOption.AllDirectories)
            .Where(path => !IsBuildOutput(path))
            .Where(path => forbidden.IsMatch(File.ReadAllText(path)))
            .Select(path => Path.GetRelativePath(tests, path).Replace('\\', '/'))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(LegacyNonPublicReflectionFiles, matches);
    }

    [Fact]
    public void Removed_runtime_methods_and_types_are_absent_from_source()
    {
        var runtime = Path.Combine(
            FindRepositoryRoot(),
            "framework/languages/dotnet/src/Zlink.Framework/Runtime"
        );
        var source = string.Join(
            Environment.NewLine,
            Directory
                .EnumerateFiles(runtime, "*.cs", SearchOption.AllDirectories)
                .Where(path => !IsBuildOutput(path))
                .Select(File.ReadAllText)
        );

        foreach (var method in RemovedRuntimeMethods)
            Assert.DoesNotMatch($@"\b{Regex.Escape(method)}\s*\(", source);
        foreach (var type in RemovedRuntimeTypes)
            Assert.DoesNotMatch($@"\b(?:class|record|struct)\s+{Regex.Escape(type)}\b", source);
    }

    [Fact]
    public void Assembly_owners_match_the_public_boundary()
    {
        var server = typeof(IZLinkFrameworkOptions).Assembly;
        var shared = typeof(IZLinkCodecExtension).Assembly;
        var provider = typeof(IZLinkLocationStore).Assembly;

        Assert.Same(server, typeof(IZLinkActor).Assembly);
        Assert.Same(server, typeof(IZLinkSpot).Assembly);
        Assert.Same(server, typeof(IZLinkRouteClient).Assembly);
        Assert.Same(server, typeof(ZLinkMessage).Assembly);

        Assert.All(
            shared.GetExportedTypes(),
            type =>
                Assert.True(
                    type.Namespace
                        is "Zlink.Framework.Contracts.Codecs"
                            or "Zlink.Framework.Contracts.Errors",
                    $"Unexpected shared contract export: {type.FullName}"
                )
        );
        Assert.Equal(8, shared.GetExportedTypes().Length);
        Assert.Null(shared.GetType("Zlink.Framework.Contracts.Codecs.IZLinkMessageCodecResolver"));
        Assert.Null(shared.GetType("Zlink.Framework.Contracts.Codecs.IZLinkMessageCodecRegistry"));
        Assert.Null(
            shared.GetType("Zlink.Framework.Contracts.Internal.ZLinkJsonSerializerOptions")
        );
        Assert.DoesNotContain(
            shared.GetReferencedAssemblies(),
            reference =>
                reference.Name
                    is "Systems.Zlink.Stream.Connector"
                        or "Systems.Zlink"
                        or "K4os.Compression.LZ4"
        );

        Assert.Equal(
            "Systems.Zlink.Stream.Connector",
            typeof(IZlinkStreamCodecRegistration).Assembly.GetName().Name
        );

        Assert.NotSame(server, provider);
        Assert.DoesNotContain(
            provider.GetReferencedAssemblies(),
            reference => reference.Name is "Zlink.Framework" or "Zlink.Framework.Contracts"
        );
        Assert.DoesNotContain(
            typeof(ZLinkHttpClient).Assembly.GetReferencedAssemblies(),
            reference =>
                reference.Name
                    is "Zlink.Framework"
                        or "Systems.Zlink.Stream.Connector"
                        or "Systems.Zlink"
                        or "K4os.Compression.LZ4"
        );
    }

    [Theory]
    [InlineData("Spots/ZLinkSpotNodeRuntime.cs")]
    [InlineData("Spots/ZLinkSpotOutboundTransport.cs")]
    [InlineData("Host/ZLinkActorBoundSessionCoordinator.cs")]
    public void One_way_submit_callers_delegate_binding_result_projection_to_Messaging(
        string relativePath
    )
    {
        var source = File.ReadAllText(
            Path.Combine(
                FindRepositoryRoot(),
                "framework/languages/dotnet/src/Zlink.Framework/Runtime",
                relativePath
            )
        );

        Assert.Contains("ZLinkSubmitFailureMapper.ToOneWayResult(", source);
        Assert.DoesNotContain("ZlinkSubmitException.ErrorCode.", source);
    }

    private static bool IsBuildOutput(string path)
    {
        var normalized = path.Replace('\\', '/');
        return normalized.Contains("/bin/", StringComparison.Ordinal)
            || normalized.Contains("/obj/", StringComparison.Ordinal);
    }

    private static string FindRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (
                File.Exists(Path.Combine(current.FullName, "AGENTS.md"))
                && File.Exists(
                    Path.Combine(current.FullName, "framework", "languages", "dotnet", "VERSION")
                )
            )
                return current.FullName;
            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not locate the repository root containing AGENTS.md."
        );
    }
}
