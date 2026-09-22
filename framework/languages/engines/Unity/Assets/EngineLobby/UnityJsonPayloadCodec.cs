using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;

namespace EngineLobby
{
    public sealed class UnityJsonPayloadCodec : IZlinkStreamPayloadCodec
    {
        public ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload)
        {
            var json = JsonUtility.ToJson(payload);
            return new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Json,
                Encoding.UTF8.GetBytes(json),
                typeof(TPayload)
            );
        }

        public TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload)
        {
            var json = Encoding.UTF8.GetString(payload.Payload.ToArray());
            return JsonUtility.FromJson<TPayload>(json);
        }
    }
}
