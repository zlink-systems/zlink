using System.Reflection;
using Zlink.Framework.Contracts.Codecs;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.HttpClient;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Zlink.Framework.ContractTests.Coverage;

public sealed class SourceLayoutContracts
{
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
        "Workers"
    ];

    [Fact]
    public void Server_application_contract_source_is_owned_by_the_server_project()
    {
        var root = FindRepositoryRoot();
        var serverContracts = Path.Combine(
            root,
            "framework/languages/dotnet/src/Zlink.Framework/Contracts");
        var sharedContracts = Path.Combine(
            root,
            "framework/languages/dotnet/src/Zlink.Framework.Contracts");

        foreach (var area in ServerContractAreas)
        {
            Assert.True(
                Directory.Exists(Path.Combine(serverContracts, area)),
                $"The Zlink.Framework project must own the {area} contract source.");
            Assert.False(
                Directory.Exists(Path.Combine(sharedContracts, area))
                && Directory.EnumerateFiles(
                    Path.Combine(sharedContracts, area),
                    "*.cs",
                    SearchOption.AllDirectories).Any(),
                $"The shared contract project must not own the {area} server contract source.");
        }

        var unexpectedSharedSources = Directory
            .EnumerateFiles(sharedContracts, "*.cs", SearchOption.AllDirectories)
            .Where(path => !IsBuildOutput(path))
            .Select(path => Path.GetRelativePath(sharedContracts, path).Replace('\\', '/'))
            .Where(path => path != "GlobalUsings.cs"
                           && !path.StartsWith("Codecs/", StringComparison.Ordinal)
                           && !path.StartsWith("Errors/", StringComparison.Ordinal)
                           && !path.StartsWith("Properties/", StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(unexpectedSharedSources);
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
            type => Assert.True(
                type.Namespace is "Zlink.Framework.Contracts.Codecs"
                    or "Zlink.Framework.Contracts.Errors",
                $"Unexpected shared contract export: {type.FullName}"));
        Assert.Equal(8, shared.GetExportedTypes().Length);
        Assert.Null(shared.GetType("Zlink.Framework.Contracts.Codecs.IZLinkMessageCodecResolver"));
        Assert.Null(shared.GetType("Zlink.Framework.Contracts.Codecs.IZLinkMessageCodecRegistry"));
        Assert.Null(shared.GetType("Zlink.Framework.Contracts.Internal.ZLinkJsonSerializerOptions"));
        Assert.DoesNotContain(
            shared.GetReferencedAssemblies(),
            reference => reference.Name is "Systems.Zlink.Stream.Connector"
                or "Systems.Zlink"
                or "K4os.Compression.LZ4");

        Assert.Equal(
            "Systems.Zlink.Stream.Connector",
            typeof(IZlinkStreamCodecRegistration).Assembly.GetName().Name);

        Assert.NotSame(server, provider);
        Assert.DoesNotContain(
            provider.GetReferencedAssemblies(),
            reference => reference.Name is "Zlink.Framework" or "Zlink.Framework.Contracts");
        Assert.DoesNotContain(
            typeof(ZLinkHttpClient).Assembly.GetReferencedAssemblies(),
            reference => reference.Name is "Zlink.Framework"
                or "Systems.Zlink.Stream.Connector"
                or "Systems.Zlink"
                or "K4os.Compression.LZ4");
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
            if (File.Exists(Path.Combine(current.FullName, "AGENTS.md")))
                return current.FullName;
            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not locate the repository root containing AGENTS.md.");
    }
}
