using RuntimeMonitoring.Server.Service.Support;

namespace RuntimeMonitoring.Server.FilteredService;

internal static class FilteredServiceHostFactory
{
    public static WebApplication Create(string[] args)
    {
        var host = ChannelMonitoringRoleHost.Create(args, "filtered-service");
        return host.Build();
    }
}
