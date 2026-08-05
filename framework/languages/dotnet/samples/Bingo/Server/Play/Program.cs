using Bingo.Server.Configuration;
using Bingo.Server.Play;
using Microsoft.Extensions.Hosting;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        var configuration = SampleConfigurationLoader.LoadPlay(args);
        using var host = PlayServerHostFactory.Build(configuration);

        await host.RunAsync();
    }

}
