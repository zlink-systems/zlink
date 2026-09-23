using System;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    public sealed class ZlinkStreamRequestSendingContext
    {
        internal ZlinkStreamRequestSendingContext(
            string requestPacketName,
            string actorId,
            ZlinkStreamMetadata metadata
        )
        {
            RequestPacketName = requestPacketName;
            ActorId = actorId;
            Metadata = metadata;
        }

        public string RequestPacketName { get; }

        public string ActorId { get; }

        internal ZlinkStreamMetadata Metadata { get; private set; }

        public void SetMetadata(string key, string value)
        {
            Metadata = Metadata.With(key, value);
        }
    }

    public sealed class ZlinkStreamReplyReceivedContext
    {
        internal ZlinkStreamReplyReceivedContext(
            string requestPacketName,
            string actorId,
            ZlinkStreamMessage<ZlinkStreamEncodedPayload> reply,
            ZlinkStreamError error,
            TimeSpan elapsed
        )
        {
            RequestPacketName = requestPacketName;
            ActorId = actorId;
            Reply = reply;
            Error = error;
            Elapsed = elapsed;
        }

        public string RequestPacketName { get; }

        public string ActorId { get; }

        public bool Succeeded => Error is null;

        public ZlinkStreamMessage<ZlinkStreamEncodedPayload> Reply { get; }

        public ZlinkStreamError Error { get; }

        public TimeSpan Elapsed { get; }
    }
}
