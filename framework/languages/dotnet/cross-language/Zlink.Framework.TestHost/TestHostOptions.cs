using System.Globalization;

internal sealed record TestHostOptions(
    string Mode,
    string? ReadyFilePath,
    string? StopFilePath,
    string? EventFilePath,
    string? RegistryPubEndpoint,
    string? RegistryRouterEndpoint,
    uint? RegistryId,
    string? ChannelName,
    string? ServerEndpoint,
    string? PublisherEndpoint,
    string? PeerRid,
    bool EnablePubSub,
    string? PublishTopic,
    string? PublishValue,
    string? StreamEndpoint,
    string? MeshName,
    string? NodeRid,
    string? BindEndpoint,
    string? PeerEndpoint,
    string? RedisEndpoint,
    string? RedisKeyPrefix,
    string? ActorId,
    string? SpotId,
    int? PayloadBytes,
    int? PlacementWeight
)
{
    public static TestHostOptions Parse(string[] args)
    {
        var parsed = new ParsedTestHostOptions();

        for (var index = 0; index < args.Length; index++)
        {
            var argument = args[index];
            if (!argument.StartsWith("--", StringComparison.Ordinal))
            {
                parsed.Mode ??= argument;
                continue;
            }

            string ReadValue()
            {
                if (index + 1 >= args.Length)
                    throw new InvalidOperationException($"Missing value for '{argument}'.");

                index++;
                return args[index];
            }

            parsed.Apply(argument, ReadValue);
        }

        parsed.ReadyFilePath ??= Environment.GetEnvironmentVariable("ZLINK_TEST_READY_FILE");
        return parsed.ToOptions();
    }

    private sealed class ParsedTestHostOptions
    {
        public string? Mode { get; set; }
        public string? ReadyFilePath { get; set; }
        public string? StopFilePath { get; set; }
        public string? EventFilePath { get; set; }
        public string? RegistryPubEndpoint { get; set; }
        public string? RegistryRouterEndpoint { get; set; }
        public uint? RegistryId { get; set; }
        public string? ChannelName { get; set; }
        public string? ServerEndpoint { get; set; }
        public string? PublisherEndpoint { get; set; }
        public string? PeerRid { get; set; }
        public bool EnablePubSub { get; set; }
        public string? PublishTopic { get; set; }
        public string? PublishValue { get; set; }
        public string? StreamEndpoint { get; set; }
        public string? MeshName { get; set; }
        public string? NodeRid { get; set; }
        public string? BindEndpoint { get; set; }
        public string? PeerEndpoint { get; set; }
        public string? RedisEndpoint { get; set; }
        public string? RedisKeyPrefix { get; set; }
        public string? ActorId { get; set; }
        public string? SpotId { get; set; }
        public int? PayloadBytes { get; set; }
        public int? PlacementWeight { get; set; }

        public void Apply(string argument, Func<string> readValue)
        {
            switch (argument)
            {
                case "--ready-file":
                    ReadyFilePath = readValue();
                    break;
                case "--stop-file":
                    StopFilePath = readValue();
                    break;
                case "--event-file":
                    EventFilePath = readValue();
                    break;
                case "--registry-pub-endpoint":
                    RegistryPubEndpoint = readValue();
                    break;
                case "--registry-router-endpoint":
                    RegistryRouterEndpoint = readValue();
                    break;
                case "--registry-id":
                    RegistryId = uint.Parse(readValue(), CultureInfo.InvariantCulture);
                    break;
                case "--channel-name":
                    ChannelName = readValue();
                    break;
                case "--server-endpoint":
                    ServerEndpoint = readValue();
                    break;
                case "--publisher-endpoint":
                    PublisherEndpoint = readValue();
                    break;
                case "--peer-rid":
                    PeerRid = readValue();
                    break;
                case "--enable-pubsub":
                    EnablePubSub = true;
                    break;
                case "--publish-topic":
                    PublishTopic = readValue();
                    break;
                case "--publish-value":
                    PublishValue = readValue();
                    break;
                case "--stream-endpoint":
                    StreamEndpoint = readValue();
                    break;
                case "--mesh-name":
                    MeshName = readValue();
                    break;
                case "--node-rid":
                    NodeRid = readValue();
                    break;
                case "--bind-endpoint":
                    BindEndpoint = readValue();
                    break;
                case "--peer-endpoint":
                    PeerEndpoint = readValue();
                    break;
                case "--redis-endpoint":
                    RedisEndpoint = readValue();
                    break;
                case "--redis-key-prefix":
                    RedisKeyPrefix = readValue();
                    break;
                case "--actor-id":
                    ActorId = readValue();
                    break;
                case "--spot-id":
                    SpotId = readValue();
                    break;
                case "--payload-bytes":
                    PayloadBytes = int.Parse(readValue(), CultureInfo.InvariantCulture);
                    break;
                case "--placement-weight":
                    PlacementWeight = int.Parse(readValue(), CultureInfo.InvariantCulture);
                    break;
            }
        }

        public TestHostOptions ToOptions()
        {
            return new TestHostOptions(
                Mode ?? "idle",
                ReadyFilePath,
                StopFilePath,
                EventFilePath,
                RegistryPubEndpoint,
                RegistryRouterEndpoint,
                RegistryId,
                ChannelName,
                ServerEndpoint,
                PublisherEndpoint,
                PeerRid,
                EnablePubSub,
                PublishTopic,
                PublishValue,
                StreamEndpoint,
                MeshName,
                NodeRid,
                BindEndpoint,
                PeerEndpoint,
                RedisEndpoint,
                RedisKeyPrefix,
                ActorId,
                SpotId,
                PayloadBytes,
                PlacementWeight
            );
        }
    }
}
