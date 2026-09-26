using System;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    public sealed class ZlinkStreamConnectorOptions
    {
        /// <summary>
        ///     The STREAM endpoint. WebGL accepts <c>ws://</c> and <c>wss://</c> only;
        ///     <c>tcp://</c> and <c>tls://</c> fail with
        ///     <see cref="ZlinkStreamErrorCode.ConfigurationError" /> when the connector is
        ///     created, not later at runtime (stream-connector spec 32 section 3.2).
        /// </summary>
        /// <remarks>
        ///     The native .NET package marks this member <c>required</c>. Unity's C# 9
        ///     compiler has no <c>required</c> modifier, so the connector factory rejects a
        ///     missing endpoint instead.
        /// </remarks>
        public Uri Endpoint { get; init; }

        public ZlinkStreamTransport? Transport { get; init; }

        public TimeSpan ConnectTimeout { get; init; } = TimeSpan.FromSeconds(5);

        public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(30);

        public TimeSpan WaitTimeout { get; init; } = TimeSpan.FromSeconds(5);

        public ZlinkStreamHeartbeatOptions Heartbeat { get; init; } =
            new ZlinkStreamHeartbeatOptions();

        public ZlinkStreamReconnectOptions Reconnect { get; init; } =
            new ZlinkStreamReconnectOptions();

        public int MaxSendPayloadSize { get; init; } = 64 * 1024;

        public int MaxReceivePayloadSize { get; init; } = 64 * 1024;

        /// <summary>
        ///     Ignored on WebGL. The browser owns TLS verification for <c>wss://</c> and
        ///     offers no way to skip it, so setting this to <c>true</c> is rejected as a
        ///     configuration error instead of being silently ignored.
        /// </summary>
        public bool SkipServerCertificateValidation { get; init; }

        public ZlinkStreamDispatchMode DispatchMode { get; init; } = ZlinkStreamDispatchMode.Manual;

        public ZlinkStreamCompression Compression { get; init; } = ZlinkStreamCompression.Lz4;

        /// <summary>
        ///     Not supported on WebGL: the compression codec lives in the JavaScript
        ///     connector and a managed codec cannot cross the jslib boundary. Select the
        ///     algorithm with <see cref="Compression" /> instead. Setting this member makes
        ///     connector creation fail with
        ///     <see cref="ZlinkStreamErrorCode.ConfigurationError" />.
        /// </summary>
        public IZlinkStreamCompressionCodec CompressionCodec { get; init; }

        public IZlinkStreamPacketNameResolver NameResolver { get; init; } =
            new ZlinkStreamPacketNameResolver();

        /// <summary>
        ///     Payload codec for the typed connector API. Unlike the native package there is
        ///     no JSON fallback (Unity ships no System.Text.Json), so the typed API throws
        ///     <see cref="NotSupportedException" /> while this is unset.
        /// </summary>
        public IZlinkStreamPayloadCodec PayloadCodec { get; init; }
    }

    public sealed class ZlinkStreamHeartbeatOptions
    {
        public bool Enabled { get; init; } = true;

        public TimeSpan Interval { get; init; } = TimeSpan.FromSeconds(1);

        public TimeSpan Timeout { get; init; } = TimeSpan.FromSeconds(5);
    }

    public sealed class ZlinkStreamReconnectOptions
    {
        public bool Enabled { get; init; } = true;

        public TimeSpan InitialDelay { get; init; } = TimeSpan.FromMilliseconds(250);

        public TimeSpan MaxDelay { get; init; } = TimeSpan.FromSeconds(5);

        public double BackoffFactor { get; init; } = 2.0;

        public int? MaxAttempts { get; init; } = 3;
    }
}
