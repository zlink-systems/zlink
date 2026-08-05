namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkTestStoreRegistrationExtensions
{
    internal static void AddLocationStore(
        this IZLinkFrameworkOptions options,
        IZLinkLocationRepository repository)
    {
        if (options is not ZLinkFrameworkOptionsBuilder builder)
            throw new ZLinkConfigurationException(
                "The test Location Store can only be registered by the framework builder.");
        builder.AddLocationRepositoryForTests(repository);
    }

    internal static void AddRelocationStore(
        this IZLinkFrameworkOptions options,
        IZLinkRelocationRepository repository)
    {
        if (options is not ZLinkFrameworkOptionsBuilder builder)
            throw new ZLinkConfigurationException(
                "The test Relocation Store can only be registered by the framework builder.");
        builder.AddRelocationRepositoryForTests(repository);
    }
}
