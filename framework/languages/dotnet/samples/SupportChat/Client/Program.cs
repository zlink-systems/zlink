using SupportChat.Client.Configuration;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Samples.Logging;

namespace SupportChat.Client;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = SupportChatClientConfiguration.Load(args);
        var streamEndpoint = configuration.StreamEndpoint;
        using var loggerFactory = SampleLogging.CreateFactory(
            configuration.LogDirectory,
            "client");
        var logger = loggerFactory.CreateLogger("SupportChat.Client");

        await using var agent = CreateClient(streamEndpoint);
        await using var customer1 = CreateClient(streamEndpoint);
        await using var customer2 = CreateClient(streamEndpoint);
        await using var reconnectingAgent = CreateClient(streamEndpoint);
        await using var reconnectingCustomer = CreateClient(streamEndpoint);
        await using var waitingCustomer = CreateClient(streamEndpoint);

        await new SupportChatClientScenario().RunAsync(
            agent,
            customer1,
            customer2,
            reconnectingAgent,
            reconnectingCustomer,
            waitingCustomer);

        logger.LogInformation("supportchat=completed");
    }

    private static IZlinkStreamConnector CreateClient(string streamEndpoint)
    {
        return ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(streamEndpoint),
            ConnectTimeout = SampleNames.ConnectTimeout,
            RequestTimeout = SampleNames.RequestTimeout,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
    }
}
