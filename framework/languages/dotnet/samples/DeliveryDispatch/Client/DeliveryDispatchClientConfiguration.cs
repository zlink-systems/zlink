using Microsoft.Extensions.Configuration;

namespace DeliveryDispatch.Client;

public sealed record DeliveryDispatchClientConfiguration(
    string LogDirectory,
    string DispatchHttpUrl,
    string CustomerStreamEndpoint,
    string CourierStreamEndpoint)
{
    public static DeliveryDispatchClientConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");

        var options = new ConfigurationBuilder()
                          .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                          .Build()
                          .GetRequiredSection("Client")
                          .Get<DeliveryDispatchClientConfiguration>()
                      ?? throw new InvalidOperationException(
                          "DeliveryDispatch client configuration is empty.");
        foreach (var property in typeof(DeliveryDispatchClientConfiguration).GetProperties())
            if (string.IsNullOrWhiteSpace((string?)property.GetValue(options)))
                throw new InvalidOperationException($"Client.{property.Name} is required.");
        return options;
    }
}
