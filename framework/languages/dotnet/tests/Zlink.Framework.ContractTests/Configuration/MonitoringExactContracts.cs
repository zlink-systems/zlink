using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

public sealed class MonitoringExactContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkClientServerRuntime))]
    public void ClientServerRuntimeHasExactChannelScopedReadOnlySurface()
    {
        Assert.Equal(
            new string[]
            {
                "GetStatus",
                "ObserveAsync",
            },
            typeof(IZLinkClientServerRuntime)
                .GetMethods()
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());
        Assert.DoesNotContain(
            typeof(IZLinkClientServerRuntime).GetMethods()
                .SelectMany(static method => method.GetParameters()),
            static parameter =>
                StringComparer.Ordinal.Equals(
                    parameter.Name,
                    "meshName"));
        Assert.Equal(
            typeof(ZLinkClientServerStatus),
            typeof(IZLinkClientServerRuntime)
                .GetMethod(nameof(IZLinkClientServerRuntime.GetStatus))!
                .ReturnType);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkDiagnosticsOptions),
        typeof(IZLinkDiagnosticsRuntime))]
    public void DiagnosticsConfigurationAndRuntimeFollowExactContract()
    {
        Assert.Equal(
            [
                ZLinkDiagnosticsLevel.Off,
                ZLinkDiagnosticsLevel.Errors,
                ZLinkDiagnosticsLevel.Normal,
                ZLinkDiagnosticsLevel.Detailed
            ],
            Enum.GetValues<ZLinkDiagnosticsLevel>());
        Assert.Equal(
            new[]
            {
                "IncludeMessageSizes",
                "SetLevel",
                "SetSampleRate"
            },
            typeof(IZLinkDiagnosticsOptions)
                .GetMethods()
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());
        var level = Assert.Single(typeof(IZLinkDiagnosticsRuntime).GetProperties());
        Assert.Equal(nameof(IZLinkDiagnosticsRuntime.Level), level.Name);
        Assert.True(level.CanRead);
        Assert.True(level.CanWrite);
    }

    [Fact]
    public void PublicMonitoringStatusOmitsInternalIdentityAndCapacity()
    {
        var names = new[]
            {
                typeof(ZLinkRouteMeshStatus),
                typeof(ZLinkClientServerStatus),
                typeof(ZLinkFanoutStatus),
                typeof(ZLinkPeerStatus)
            }
            .SelectMany(static type => type.GetProperties())
            .Select(static property => property.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("Endpoint", names);
        Assert.DoesNotContain("LifecycleGeneration", names);
        Assert.DoesNotContain("DescriptorRevision", names);
        Assert.DoesNotContain("Capacity", names);
    }
}
