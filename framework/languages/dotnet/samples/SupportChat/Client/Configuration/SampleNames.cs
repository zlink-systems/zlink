namespace SupportChat.Client.Configuration;

using Microsoft.Extensions.Configuration;

public static class SampleNames
{
    // Stream metadata key that carries ConversationId for session routing (§9.2).
    public const string ConversationIdMetadataKey = "ConversationId";

    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(3);
    public static readonly TimeSpan CloseGraceTimeout = TimeSpan.FromSeconds(2);
}

public static class SupportChatRoles
{
    public const string Customer = "Customer";
    public const string Agent = "Agent";
}

public static class ConversationStatuses
{
    public const string WaitingForAgent = "WaitingForAgent";
    public const string Active = "Active";
    public const string WaitingForClose = "WaitingForClose";
    public const string Closed = "Closed";
}

public sealed record SupportChatClientConfiguration(
    string LogDirectory,
    string StreamEndpoint)
{
    public static SupportChatClientConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var options = new ConfigurationBuilder()
                          .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                          .Build()
                          .GetRequiredSection("Client")
                          .Get<SupportChatClientConfiguration>()
                      ?? throw new InvalidOperationException("SupportChat client configuration is empty.");
        if (string.IsNullOrWhiteSpace(options.LogDirectory))
            throw new InvalidOperationException("Client.LogDirectory is required.");
        if (string.IsNullOrWhiteSpace(options.StreamEndpoint))
            throw new InvalidOperationException("Client.StreamEndpoint is required.");
        return options;
    }
}
