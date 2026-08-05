using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Configuration;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Samples.Logging;

namespace TicTacToe.Client;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = TicTacToeClientConfiguration.Load(args);
        var options = TicTacToeClientOptions.CreateDefault() with
        {
            ApiUrl = new Uri(configuration.ApiPublicUrls[0])
        };
        using var loggerFactory = SampleLogging.CreateFactory(
            configuration.LogDirectory,
            "client");
        var logger = loggerFactory.CreateLogger("TicTacToe.Client");
        await new TicTacToeClientScenario(logger).RunAsync(options);
        logger.LogInformation("tictactoe=completed");
    }
}

internal sealed class TicTacToeClientConfiguration
{
    public string[] ApiPublicUrls { get; set; } = [];

    public string LogDirectory { get; set; } = string.Empty;

    public static TicTacToeClientConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var settings = new ConfigurationBuilder()
            .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
            .Build()
            .GetRequiredSection("Sample")
            .Get<TicTacToeClientConfiguration>()
            ?? throw new InvalidOperationException("Sample client configuration is missing.");
        if (settings.ApiPublicUrls.Length == 0
            || !Uri.TryCreate(settings.ApiPublicUrls[0], UriKind.Absolute, out _))
            throw new InvalidOperationException("Sample.ApiPublicUrls[0] is required.");
        if (string.IsNullOrWhiteSpace(settings.LogDirectory))
            throw new InvalidOperationException("Sample.LogDirectory is required.");
        return settings;
    }
}

public sealed record TicTacToeClientOptions(
    Uri ApiUrl,
    string GameName,
    string XActorId,
    string OActorId,
    string ObserverActorId,
    TimeSpan HttpTimeout,
    TimeSpan StreamTimeout)
{
    public static TicTacToeClientOptions CreateDefault()
    {
        return new TicTacToeClientOptions(
            new Uri("http://127.0.0.1:18080"),
            "tictactoe-game",
            "player-x",
            "player-o",
            "observer",
            TimeSpan.FromSeconds(10),
            TimeSpan.FromSeconds(5));
    }
}

public static class TicTacToeClientConnections
{
    public static IZlinkStreamConnector CreateStreamClient(
        string streamEndpoint,
        TicTacToeClientOptions options,
        string role,
        ILogger logger)
    {
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(streamEndpoint),
            ConnectTimeout = options.StreamTimeout,
            RequestTimeout = options.StreamTimeout,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        connector.ObserveInbound((observation, _) =>
        {
            logger.LogInformation(
                "stream-inbound sample=TicTacToe client={0} kind={1} name={2} seq={3} bytes={4}",
                role,
                observation.Kind,
                observation.Name,
                observation.RequestSeq?.ToString() ?? "-",
                observation.PayloadLength);
            return ValueTask.CompletedTask;
        });
        return connector;
    }
}
