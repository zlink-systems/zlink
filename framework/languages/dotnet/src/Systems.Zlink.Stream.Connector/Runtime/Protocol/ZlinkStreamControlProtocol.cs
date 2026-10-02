namespace Systems.Zlink.Stream.Connector.Runtime.Protocol;

internal static class ZlinkStreamControlProtocol
{
    internal const string BoundControlName = "$zlink.actor.bound";
    internal const string UnboundControlName = "$zlink.actor.unbound";
    internal const string HeartbeatPingName = "$zlink.heartbeat.ping";
    internal const string HeartbeatPongName = "$zlink.heartbeat.pong";
    internal const byte ActorControlVersion = 1;
    internal const int ActorSlotOffset = sizeof(byte);
    internal const int ActorSlotSize = sizeof(ushort);
    internal const int ActorIdLengthOffset = ActorSlotOffset + ActorSlotSize;
    internal const int ActorBoundPrefixSize = ActorIdLengthOffset + sizeof(byte);
    internal const int ActorBoundMinimumSize = ActorBoundPrefixSize + sizeof(byte);
    internal const int ActorUnboundPayloadSize = ActorSlotOffset + ActorSlotSize;
}
