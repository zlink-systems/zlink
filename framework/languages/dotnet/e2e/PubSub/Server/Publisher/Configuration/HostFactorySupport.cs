using Microsoft.Extensions.Configuration;

namespace PubSub.Server.Publisher.Configuration;

public static class HostFactorySupport
{
    public static WebApplicationBuilder CreateBuilder(string[] args, string httpUrl, string logDir)
    {
        Directory.CreateDirectory(logDir);

        var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole(console =>
        {
            console.SingleLine = true;
            console.TimestampFormat = "HH:mm:ss.fff ";
        });
        builder.WebHost.UseUrls(httpUrl);
        return builder;
    }
}
