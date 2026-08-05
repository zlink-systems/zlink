using Bingo.Server.Api;
using Bingo.Server.Configuration;
using Microsoft.Extensions.Hosting;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        var configuration = SampleConfigurationLoader.LoadApi(args);
        await ApiServerHostFactory.Build(configuration)
            .RunAsync();
    }
}
