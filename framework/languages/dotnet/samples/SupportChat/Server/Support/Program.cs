using Microsoft.Extensions.Hosting;
using SupportChat.Server.Configuration;
using SupportChat.Server.Support;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        var configuration = SampleTopology.LoadSupport(args);
        await SupportServerHostFactory.Build(configuration.Topology, configuration.LogDirectory)
            .RunAsync();
    }
}
