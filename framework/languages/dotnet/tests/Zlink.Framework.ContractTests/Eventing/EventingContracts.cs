using Zlink.Framework.AspNetCore;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Monitoring;

public sealed class EventingContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkFrameworkRuntime))]
    public void Monitoring_uses_typed_status_and_standard_diagnostics_only()
    {
        var frameworkAssembly = typeof(IZLinkFrameworkRuntime).Assembly;
        foreach (var name in new[]
                 {
                     "IZLinkMonitoringOptions",
                     "IZLinkRuntimeEvent",
                     "IZLinkRuntimeEventHandler`1",
                     "ZLinkSocketEvent",
                     "ZLinkSocketEventKind",
                     "ZLinkLocationRuntimeEvent",
                     "ZLinkSpotEvent",
                     "ZLinkSpotTimerDiagnostic"
                 })
        {
            var type = frameworkAssembly.GetType(
                $"Zlink.Framework.Contracts.Eventing.{name}");
            Assert.True(type is null || !type.IsVisible, $"{name} must not be public.");
        }

        Assert.Null(typeof(ServiceCollectionExtensions).GetMethod("AddZLinkMonitoring"));
        AssertEnumValues<ZLinkDiagnosticsLevel>(
            ("Off", 0), ("Errors", 1), ("Normal", 2), ("Detailed", 3));
    }

    [Fact]
    [ContractExample(typeof(IZLinkFrameworkRuntime))]
    public void Termination_contract_matches_the_exact_surface()
    {
        var contract = typeof(IZLinkFrameworkRuntime);
        var status = contract.GetProperty(nameof(IZLinkFrameworkRuntime.Status));
        Assert.NotNull(status);
        Assert.Equal(typeof(ZLinkFrameworkRuntimeStatus), status!.PropertyType);
        Assert.True(status.CanRead);
        Assert.False(status.CanWrite);

        Assert.NotNull(contract.GetMethod(nameof(IZLinkFrameworkRuntime.RelocateAsync)));
        Assert.NotNull(contract.GetMethod(nameof(IZLinkFrameworkRuntime.ShutdownAsync)));
        Assert.Null(contract.GetMethod("RetireAsync"));
        Assert.Null(contract.GetMethod("DrainAsync"));
        Assert.Null(contract.GetMethod("AwaitDrainedAsync"));

        var healthExtension = typeof(ServiceCollectionExtensions).GetMethod(
            nameof(ServiceCollectionExtensions.AddZLinkDrainHealthCheck),
            [typeof(Microsoft.Extensions.DependencyInjection.IHealthChecksBuilder)]);
        Assert.NotNull(healthExtension);

        Assert.Null(contract.Assembly.GetType(
            "Zlink.Framework.Contracts.Dispatch.ZLinkRuntimeMessageFlowEvent"));
    }

    private static void AssertEnumValues<TEnum>(params (string Name, int Value)[] expected)
        where TEnum : struct, Enum =>
        Assert.Equal(
            expected,
            Enum.GetValues<TEnum>()
                .Select(static value => (value.ToString(), Convert.ToInt32(value)))
                .ToArray());
}
