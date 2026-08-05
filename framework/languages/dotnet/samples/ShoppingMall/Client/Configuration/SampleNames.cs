namespace ShoppingMall.Client.Configuration;

using Microsoft.Extensions.Configuration;

public static class OrderStatuses
{
    public const string Created = "Created";
    public const string InventoryReserved = "InventoryReserved";
    public const string PaymentAuthorized = "PaymentAuthorized";
    public const string Confirmed = "Confirmed";
    public const string Failed = "Failed";
}

public static class SampleTimings
{
    public static readonly TimeSpan HttpTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan WorkflowTimeout = TimeSpan.FromSeconds(8);
}

public sealed record ShoppingMallClientConfiguration(
    string LogDirectory,
    string ApiAHttpUrl,
    string ApiBHttpUrl)
{
    public static ShoppingMallClientConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var options = new ConfigurationBuilder()
                          .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                          .Build()
                          .GetRequiredSection("Client")
                          .Get<ShoppingMallClientConfiguration>()
                      ?? throw new InvalidOperationException("ShoppingMall client configuration is empty.");
        foreach (var property in typeof(ShoppingMallClientConfiguration).GetProperties())
            if (string.IsNullOrWhiteSpace((string?)property.GetValue(options)))
                throw new InvalidOperationException($"Client.{property.Name} is required.");
        return options;
    }
}
