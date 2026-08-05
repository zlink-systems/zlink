using Zlink.Framework.ContractTests.Support;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.ContractTests.Locations;

public sealed class ProviderStoreContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkLocationStore))]
    public void Location_provider_exposes_only_opaque_store_operations()
    {
        Assert.Equal(
            new[] { "ReadAsync", "ScanAsync", "WriteAsync" },
            typeof(IZLinkLocationStore).GetMethods()
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());

        var providerAssembly = typeof(IZLinkLocationStore).Assembly;
        Assert.DoesNotContain(
            providerAssembly.GetExportedTypes(),
            static type => type.Name.Contains("Authority", StringComparison.Ordinal)
                           || type.Name.Contains("Lease", StringComparison.Ordinal)
                           || type.Name.Contains("Aggregate", StringComparison.Ordinal)
                           || type.Name.Contains("Capacity", StringComparison.Ordinal));
    }

    [Fact]
    [ContractExample(typeof(IZLinkRelocationStore))]
    public void Relocation_provider_exposes_only_immutable_blob_operations()
    {
        Assert.Equal(
            new[] { "DeleteAsync", "PutAsync", "ReadAsync", "RenewAsync" },
            typeof(IZLinkRelocationStore).GetMethods()
                .Select(static method => method.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());
        Assert.Equal(
            typeof(ZLinkBlobReference),
            typeof(IZLinkRelocationStore).GetMethod("PutAsync")!
                .GetParameters()[0].ParameterType);
    }
}
