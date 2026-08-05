using GameQuest.Client.Configuration;
using Systems.Zlink.Stream.Connector.Contracts;

namespace GameQuest.Client;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var topology = GameQuestTopology.Load(args);
        await using var apiA = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(topology.GameApiAStreamEndpoint),
            RequestTimeout = SampleNames.RequestTimeout,
            ConnectTimeout = SampleNames.RequestTimeout,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await using var apiB = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(topology.GameApiBStreamEndpoint),
            RequestTimeout = SampleNames.RequestTimeout,
            ConnectTimeout = SampleNames.RequestTimeout,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });

        await new GameQuestClientScenario(topology).RunAsync(apiA, apiB);
    }
}
