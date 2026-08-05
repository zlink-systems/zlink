using Microsoft.Extensions.Hosting;
using SupportChat.Server.Configuration;
using SupportChat.Server.Session;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        var configuration = SampleTopology.LoadSession(args);
        await SessionServerHostFactory.Build(
                configuration.Topology,
                configuration.Topology.PrimarySession,
                configuration.LogDirectory)
            .RunAsync();
    }
}
