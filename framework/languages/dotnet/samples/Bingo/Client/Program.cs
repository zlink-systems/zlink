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
        using var loggerFactory = SampleLogging.CreateFactory(
            configuration.LogDirectory,
            "client");
        var logger = loggerFactory.CreateLogger("Bingo.Client");

        await using var client1 = CreateClient(streamAEndpoint, "player1", logger);
        await using var client2 = CreateClient(streamBEndpoint, "player2", logger);
        await using var observer = CreateClient(streamBEndpoint, "observer", logger);

        await new BingoClientScenario().RunAsync(
            client1,
            client2,
            observer);
        logger.LogInformation("bingo=completed");
    }

    private static IZlinkStreamConnector CreateClient(
        string streamEndpoint,
        string clientName,
        ILogger logger)
    {
        return ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(streamEndpoint),
                ConnectTimeout = SampleTimings.ConnectTimeout,
                RequestTimeout = SampleTimings.RequestTimeout,
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                PayloadCodec = ZLinkProtobufCodec.Default
            })
            .WithInboundObserver((observation, _) =>
            {
                logger.LogInformation(
                    "stream-inbound sample=Bingo client={0} kind={1} name={2} seq={3} bytes={4}",
                    clientName,
                    observation.Kind,
                    observation.Name,
                    observation.RequestSeq?.ToString() ?? "-",
                    observation.PayloadLength);
                return ValueTask.CompletedTask;
            });
    }
}
