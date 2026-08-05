using DeliveryDispatch.Client;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;
using Zlink.Samples.Logging;

var configuration = DeliveryDispatchClientConfiguration.Load(args);
using var loggerFactory = SampleLogging.CreateFactory(configuration.LogDirectory, "client");
var logger = loggerFactory.CreateLogger("DeliveryDispatch.Client");

using var http = ZLinkHttpClient.Create(configuration.DispatchHttpUrl).Build();
await using var customer = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri(configuration.CustomerStreamEndpoint),
    ConnectTimeout = TimeSpan.FromSeconds(5),
    RequestTimeout = TimeSpan.FromSeconds(5),
    WaitTimeout = TimeSpan.FromSeconds(15),
    DispatchMode = ZlinkStreamDispatchMode.Immediate
});
await using var courierA = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri(configuration.CourierStreamEndpoint),
    ConnectTimeout = TimeSpan.FromSeconds(5),
    RequestTimeout = TimeSpan.FromSeconds(5),
    WaitTimeout = TimeSpan.FromSeconds(15),
    DispatchMode = ZlinkStreamDispatchMode.Immediate
});
await using var courierB = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
{
    Endpoint = new Uri(configuration.CourierStreamEndpoint),
    ConnectTimeout = TimeSpan.FromSeconds(5),
    RequestTimeout = TimeSpan.FromSeconds(5),
    WaitTimeout = TimeSpan.FromSeconds(15),
    DispatchMode = ZlinkStreamDispatchMode.Immediate
});

await new DeliveryDispatchClientScenario(logger).RunAsync(http, customer, courierA, courierB);

logger.LogInformation("deliverydispatch=completed");
