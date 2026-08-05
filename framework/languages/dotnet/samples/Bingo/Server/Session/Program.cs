using Bingo.Server.Configuration;
using Bingo.Server.Session;
using Microsoft.Extensions.Hosting;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        var configuration = SampleConfigurationLoader.LoadSession(args);

        await SessionServerHostFactory.Build(configuration)
            .RunAsync();
    }
}
