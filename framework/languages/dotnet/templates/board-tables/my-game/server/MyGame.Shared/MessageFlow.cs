using System.Diagnostics;
using System.Text.Json;

namespace MyGame.Shared;

public sealed class MessageFlow : IDisposable
{
    private readonly ActivityListener listener = new()
    {
        ShouldListenTo = source => source.Name == "Zlink.Framework",
        Sample = (ref ActivityCreationOptions<ActivityContext> _) => ActivitySamplingResult.AllData,
        ActivityStopped = activity =>
            Console.Error.WriteLine(
                JsonSerializer.Serialize(
                    new
                    {
                        Event = activity.OperationName,
                        Started = activity.StartTimeUtc,
                        Tags = activity.TagObjects.ToDictionary(tag => tag.Key, tag => tag.Value),
                    }
                )
            ),
    };

    public MessageFlow() => ActivitySource.AddActivityListener(listener);

    public void Dispose() => listener.Dispose();
}
