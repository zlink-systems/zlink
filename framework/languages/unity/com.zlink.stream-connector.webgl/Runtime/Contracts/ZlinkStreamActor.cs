using System;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts.Calls;
using Systems.Zlink.Stream.Connector.Runtime;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    /// <summary>Represents one Actor currently bound to the stream session.</summary>
    public sealed class ZlinkStreamActor
    {
        private readonly ZlinkStreamWebGlConnector _connector;

        internal ZlinkStreamActor(ZlinkStreamWebGlConnector connector, string actorId)
        {
            _connector = connector;
            ActorId = actorId;
            IsBound = true;
        }

        public string ActorId { get; }

        public bool IsBound { get; private set; }

        public IZlinkStreamSendCall Send(object payload)
        {
            EnsureBound();
            return _connector.SendActor(this, Encode(payload));
        }

        public IZlinkStreamRequestCall Request(object payload)
        {
            EnsureBound();
            return _connector.RequestActor(this, Encode(payload));
        }

        public IDisposable On<TPayload>(string name, Action<ZlinkStreamMessage<TPayload>> handler)
        {
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            return _connector.OnActor(
                this,
                name,
                (message, _) =>
                {
                    var payload = ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(
                        _connector.Options.PayloadCodec,
                        message.Payload
                    );
                    handler(
                        new ZlinkStreamMessage<TPayload>(
                            message.Name,
                            message.Metadata,
                            payload,
                            message.ActorId
                        )
                    );
                    return default;
                }
            );
        }

        internal void Close()
        {
            IsBound = false;
        }

        internal void EnsureBound()
        {
            if (!IsBound)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    $"Actor '{ActorId}' is no longer bound."
                );
        }

        private ZlinkStreamEncodedPayload Encode(object payload)
        {
            if (payload is ZlinkStreamEncodedPayload encoded)
                return encoded;
            return ZlinkStreamTypedConnectorExtensions.EncodePayload(
                _connector.Options.PayloadCodec,
                payload
            );
        }
    }
}
