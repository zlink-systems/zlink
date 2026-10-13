using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Locations.Redis;

namespace MyGame.Shared;

public static class GameHosting
{
    public static void Configure(
        HostApplicationBuilder builder,
        NodeSettings settings,
        Action<IZLinkFrameworkOptions> configure
    )
    {
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace);
        builder.Logging.AddSimpleConsole(options => options.SingleLine = true);
        builder.Services.AddSingleton(settings);
        builder.Services.AddSingleton(_ => new MessageFlowFile(settings.Id));
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(
                new ZLinkRedisLocationStore(redis =>
                {
                    redis.ConnectionString = settings.RedisEndpoint;
                    redis.KeyPrefix = settings.KeyPrefix;
                })
            );
            options.AddRelocationStore(
                new ZLinkRedisRelocationStore(redis =>
                {
                    redis.ConnectionString = settings.RedisEndpoint;
                    redis.KeyPrefix = settings.KeyPrefix + "relocation:";
                })
            );
            options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            configure(options);
        });
    }
}

// ActivitySource is the public diagnostics surface. This lock only serializes file writes.
internal sealed class MessageFlowFile : IDisposable
{
    private readonly object _fileGate = new();
    private readonly StreamWriter _writer;
    private readonly ActivityListener _listener;

    public MessageFlowFile(string nodeId)
    {
        var logDirectory =
            Environment.GetEnvironmentVariable(NodeProtocol.LogDirectoryEnvironmentVariable)
            ?? ".logs";
        Directory.CreateDirectory(logDirectory);
        _writer = new StreamWriter(Path.Combine(logDirectory, nodeId + ".flow.jsonl"))
        {
            AutoFlush = true,
        };
        _listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == "Zlink.Framework",
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = activity =>
            {
                var line = JsonSerializer.Serialize(
                    new
                    {
                        NodeId = nodeId,
                        Event = activity.OperationName,
                        Started = activity.StartTimeUtc,
                        Tags = activity.TagObjects.ToDictionary(tag => tag.Key, tag => tag.Value),
                    }
                );
                lock (_fileGate)
                    _writer.WriteLine(line);
            },
        };
        ActivitySource.AddActivityListener(_listener);
    }

    public void Dispose()
    {
        _listener.Dispose();
        lock (_fileGate)
            _writer.Dispose();
    }
}
