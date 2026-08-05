namespace Bingo.Client.Configuration;

using Microsoft.Extensions.Configuration;

public static class SampleTimings
{
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
}

public sealed record BingoClientConfiguration(
    string LogDirectory,
    string SessionAStreamEndpoint,
    string SessionBStreamEndpoint)
{
    public static BingoClientConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");

        var options = new ConfigurationBuilder()
                          .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                          .Build()
                          .GetRequiredSection("Client")
                          .Get<BingoClientConfiguration>()
                      ?? throw new InvalidOperationException("Bingo client configuration is empty.");
        Require(options.LogDirectory, nameof(LogDirectory));
        Require(options.SessionAStreamEndpoint, nameof(SessionAStreamEndpoint));
        Require(options.SessionBStreamEndpoint, nameof(SessionBStreamEndpoint));
        return options;
    }

    private static void Require(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"Client.{name} is required.");
    }
}
