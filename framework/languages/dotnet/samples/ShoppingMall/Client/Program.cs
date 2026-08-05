using ShoppingMall.Client.Configuration;
using Microsoft.Extensions.Logging;
using Zlink.HttpClient;
using Zlink.Samples.Logging;

namespace ShoppingMall.Client;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = ShoppingMallClientConfiguration.Load(args);
        using var loggerFactory = SampleLogging.CreateFactory(
            configuration.LogDirectory,
            "client");
        var logger = loggerFactory.CreateLogger("ShoppingMall.Client");
        using var apiA = ZLinkHttpClient.Create(configuration.ApiAHttpUrl)
            .Timeout(SampleTimings.HttpTimeout)
            .Build();
        using var apiB = ZLinkHttpClient.Create(configuration.ApiBHttpUrl)
            .Timeout(SampleTimings.HttpTimeout)
            .Build();

        await new ShoppingMallClientScenario().RunAsync(apiA, apiB, CancellationToken.None);
        logger.LogInformation("shoppingmall=completed");
    }
}
