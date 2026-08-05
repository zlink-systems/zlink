using ShoppingMall.Server.Configuration;

namespace ShoppingMall.Server.OrderWorkflow;

internal static class Program
{
    public static async Task Main(string[] args)
    {
        var configuration = SampleTopology.LoadWorkflow(args);
        var instance = configuration.Topology.ForWorkflowInstance(configuration.InstanceId);
        await using var app = OrderWorkflowServerHostFactory.Build(
            configuration.Topology,
            instance,
            configuration.LogDirectory,
            args);
        await app.RunAsync();
    }
}
