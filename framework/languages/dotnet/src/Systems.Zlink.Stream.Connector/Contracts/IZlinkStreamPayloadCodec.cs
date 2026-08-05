namespace Systems.Zlink.Stream.Connector.Contracts;

/// <summary>
///     Converts a business object to and from a <see cref="ZlinkStreamEncodedPayload" />.
///     Set <see cref="ZlinkStreamConnectorOptions.PayloadCodec" /> to plug a custom codec
///     (for example Avro or Thrift) into the typed connector send/request/observe API.
///     When left unset, the connector uses JSON.
/// </summary>
public interface IZlinkStreamPayloadCodec
{
    /// <summary>Encodes <paramref name="payload" /> into a wire payload.</summary>
    ZlinkStreamEncodedPayload Encode<TPayload>(TPayload payload);

    /// <summary>Decodes a wire payload back into an instance of <typeparamref name="TPayload" />.</summary>
    TPayload Decode<TPayload>(ZlinkStreamEncodedPayload payload);
}

/// <summary>
///     Describes the STREAM wire codec supplied by a payload codec extension.
///     Framework packages use this descriptor to map a serializer content type to
///     the STREAM header value without adding STREAM concerns to the shared serializer registry.
/// </summary>
public interface IZlinkStreamCodecRegistration
{
    string ContentType { get; }

    ZlinkStreamCodec Codec { get; }
}
