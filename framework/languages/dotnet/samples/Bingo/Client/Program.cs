using Bingo.Client.Configuration;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Samples.Logging;

namespace Bingo.Client;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = BingoClientConfiguration.Load(args);
        var streamAEndpoint = configuration.SessionAStreamEndpoint;
        var streamBEndpoint = configuration.SessionBStreamEndpoint;
        using var loggerFactory = SampleLogging.CreateFactory(configuration.LogDirectory, "client");
        var logger = loggerFactory.CreateLogger("Bingo.Client");

        await using var client1 = CreateClient(streamAEndpoint);
        await using var client2 = CreateClient(streamBEndpoint);
        await using var observer = CreateClient(streamBEndpoint);

        await new BingoClientScenario(logger).RunAsync(client1, client2, observer);
        logger.LogInformation("bingo=completed");
    }

    private static IZlinkStreamConnector CreateClient(string streamEndpoint)
    {
        return ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(streamEndpoint),
                ConnectTimeout = SampleTimings.ConnectTimeout,
                RequestTimeout = SampleTimings.RequestTimeout,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                PayloadCodec = ZLinkProtobufCodec.Default,
            }
        );
    }
}
