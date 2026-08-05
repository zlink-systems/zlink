using PubSub.Shared;
using Zlink.HttpClient;

namespace PubSub.Client.Support;

internal static class SubscriberObservation
{
    public static async Task<int> EvidenceCountAsync(ZLinkHttpClient subscriber)
    {
        return (await subscriber.Get("/evidence").Async<string[]>()).Body.Length;
    }

    public static async Task<string[]> WaitForEventAsync(
        ZLinkHttpClient subscriber,
        string runId,
        int sequence)
    {
        return (await subscriber.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([], [])
            {
                ContainsAllLineGroups =
                [
                    [
                        "event|",
                        $"run={runId}",
                        $"topic={PubSubNames.MainTopic}",
                        $"seq={sequence}|"
                    ]
                ]
            })
            .Async<string[]>()).Body;
    }
}
