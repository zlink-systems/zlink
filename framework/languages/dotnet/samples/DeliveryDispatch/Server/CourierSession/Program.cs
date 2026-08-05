using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Server.CourierSession;
using Microsoft.Extensions.Hosting;

internal static class Program
{
    private static async Task Main(string[] args)
    {
        // The role gets its configuration file and nothing else. No environment variable is read
        // anywhere below this line
        // (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md §2).
        var configuration = SampleConfiguration.Load(args);
        await CourierSessionHostFactory.Build(configuration).RunAsync();
    }
}
