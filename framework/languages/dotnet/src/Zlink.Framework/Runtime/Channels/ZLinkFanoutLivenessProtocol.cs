namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkFanoutLivenessProtocol
{
    internal const string Topic = "\u0001ZLF1";
    internal static readonly byte[] Payload = [0x5A, 0x46, 0x01, 0x01];
    internal static readonly TimeSpan BeaconInterval = TimeSpan.FromSeconds(5);
    internal static readonly TimeSpan InboundTimeout = TimeSpan.FromSeconds(15);

    internal static bool IsReservedTopic(string topic) =>
        string.Equals(topic, Topic, StringComparison.Ordinal);

    internal static bool IsValidBeacon(TopicMessage message) =>
        IsValidBeacon(message.Topic, message.Parts);

    internal static bool IsValidBeacon(
        string topic,
        IReadOnlyList<Message> parts) =>
        IsReservedTopic(topic)
        && parts.Count == 1
        && parts[0].AsReadOnlySpan().SequenceEqual(Payload);

    internal static bool IsInboundTimedOut(
        DateTimeOffset lastActivity,
        DateTimeOffset now) =>
        now - lastActivity >= InboundTimeout;
}
