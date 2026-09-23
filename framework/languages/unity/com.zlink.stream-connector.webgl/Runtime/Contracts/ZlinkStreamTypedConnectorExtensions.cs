// No ConfigureAwait(false) in this package: a WebGL player has no thread pool, so a
// continuation that did not capture the context is queued and never runs. See the
// remarks on ZlinkStreamWebGlConnector.
using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts.Calls;

namespace Systems.Zlink.Stream.Connector.Contracts
{
    /// <summary>
    ///     Typed send, request and observe surface over the encoded connector API.
    /// </summary>
    /// <remarks>
    ///     Same method names and shapes as the native .NET package with one difference:
    ///     there is no JSON fallback. The native package falls back to System.Text.Json,
    ///     which Unity does not ship, and bundling a serializer into a connector adapter
    ///     would pick the game's serializer for it. Set
    ///     <see cref="ZlinkStreamConnectorOptions.PayloadCodec" /> to use this surface;
    ///     otherwise every typed call throws <see cref="NotSupportedException" />. The
    ///     encoded surface on <see cref="IZlinkStreamConnector" /> needs no codec.
    /// </remarks>
    public static class ZlinkStreamTypedConnectorExtensions
    {
        public static int ReceivedCount<TPayload>(this IZlinkStreamConnector connector)
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return connector.ReceivedCount(
                connector.Options.NameResolver.Resolve(typeof(TPayload))
            );
        }

        public static ZlinkStreamTypedSendBuilder Send<TPayload>(
            this IZlinkStreamConnector connector,
            TPayload payload
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            var encoded = EncodePayload(connector.Options.PayloadCodec, payload);
            return new ZlinkStreamTypedSendBuilder(
                connector
                    .Send(encoded)
                    .PacketName(connector.Options.NameResolver.Resolve(typeof(TPayload)))
            );
        }

        public static ZlinkStreamTypedRequestBuilder Request<TPayload>(
            this IZlinkStreamConnector connector,
            TPayload payload
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            var encoded = EncodePayload(connector.Options.PayloadCodec, payload);
            return new ZlinkStreamTypedRequestBuilder(
                connector
                    .Request(encoded)
                    .PacketName(connector.Options.NameResolver.Resolve(typeof(TPayload))),
                connector.Options.PayloadCodec
            );
        }

        public static IDisposable On<TPayload>(
            this IZlinkStreamConnector connector,
            Func<ZlinkStreamMessage<TPayload>, CancellationToken, ValueTask> handler
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return connector.On(connector.Options.NameResolver.Resolve(typeof(TPayload)), handler);
        }

        public static IDisposable On<TPayload>(
            this IZlinkStreamConnector connector,
            string name,
            Func<ZlinkStreamMessage<TPayload>, CancellationToken, ValueTask> handler
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            return connector.On(
                name,
                (message, cancellationToken) =>
                {
                    var payload = DecodePayload<TPayload>(
                        connector.Options.PayloadCodec,
                        message.Payload
                    );
                    return handler(
                        new ZlinkStreamMessage<TPayload>(
                            message.Name,
                            message.Metadata,
                            payload,
                            message.ActorId
                        ),
                        cancellationToken
                    );
                }
            );
        }

        public static ZlinkStreamTypedWaitBuilder<TPayload> WaitFor<TPayload>(
            this IZlinkStreamConnector connector,
            string name
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return new ZlinkStreamTypedWaitBuilder<TPayload>(
                connector.WaitFor(name),
                connector.Options.PayloadCodec
            );
        }

        public static ZlinkStreamTypedWaitBuilder<TPayload> WaitFor<TPayload>(
            this IZlinkStreamConnector connector
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return connector.WaitFor<TPayload>(
                connector.Options.NameResolver.Resolve(typeof(TPayload))
            );
        }

        public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(
            this IZlinkStreamConnector connector,
            string name
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return new ZlinkStreamTypedExpectNoneBuilder<TPayload>(connector.ExpectNone(name));
        }

        public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(
            this IZlinkStreamConnector connector
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return connector.ExpectNone<TPayload>(
                connector.Options.NameResolver.Resolve(typeof(TPayload))
            );
        }

        public static ZlinkStreamTypedSequenceBuilder<TPayload> WaitForSequence<TPayload>(
            this IZlinkStreamConnector connector,
            string name
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return new ZlinkStreamTypedSequenceBuilder<TPayload>(
                connector.WaitForSequence(name),
                connector.Options.PayloadCodec
            );
        }

        public static ZlinkStreamTypedSequenceBuilder<TPayload> WaitForSequence<TPayload>(
            this IZlinkStreamConnector connector
        )
        {
            if (connector is null)
                throw new ArgumentNullException(nameof(connector));
            return connector.WaitForSequence<TPayload>(
                connector.Options.NameResolver.Resolve(typeof(TPayload))
            );
        }

        internal static ZlinkStreamEncodedPayload EncodePayload<TPayload>(
            IZlinkStreamPayloadCodec codec,
            TPayload payload
        )
        {
            return RequireCodec(codec).Encode(payload);
        }

        internal static TPayload DecodePayload<TPayload>(
            IZlinkStreamPayloadCodec codec,
            ZlinkStreamEncodedPayload payload
        )
        {
            return RequireCodec(codec).Decode<TPayload>(payload);
        }

        private static IZlinkStreamPayloadCodec RequireCodec(IZlinkStreamPayloadCodec codec)
        {
            if (codec != null)
                return codec;
            throw new NotSupportedException(
                "The typed stream API needs ZlinkStreamConnectorOptions.PayloadCodec on WebGL. "
                    + "Unity ships no System.Text.Json, so this package has no JSON fallback. "
                    + "Set a codec, or use the encoded API on IZlinkStreamConnector."
            );
        }
    }

    /// <summary>Configures a typed negative observation for one packet name.</summary>
    public sealed class ZlinkStreamTypedExpectNoneBuilder<TPayload>
    {
        private readonly IZlinkStreamExpectNoneCall _inner;

        internal ZlinkStreamTypedExpectNoneBuilder(IZlinkStreamExpectNoneCall inner)
        {
            _inner = inner;
        }

        public ZlinkStreamTypedExpectNoneBuilder<TPayload> Within(TimeSpan window)
        {
            _inner.Within(window);
            return this;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            return _inner.Async(cancellationToken);
        }
    }

    /// <summary>Configures typed predicates that must match messages in arrival order.</summary>
    public sealed class ZlinkStreamTypedSequenceBuilder<TPayload>
    {
        private readonly IZlinkStreamPayloadCodec _codec;
        private readonly IZlinkStreamSequenceCall _inner;

        internal ZlinkStreamTypedSequenceBuilder(
            IZlinkStreamSequenceCall inner,
            IZlinkStreamPayloadCodec codec
        )
        {
            _inner = inner;
            _codec = codec;
        }

        public ZlinkStreamTypedSequenceBuilder<TPayload> Expect(
            Func<ZlinkStreamMessage<TPayload>, bool> predicate
        )
        {
            if (predicate is null)
                throw new ArgumentNullException(nameof(predicate));
            _inner.Expect(message => predicate(Decode(message)));
            return this;
        }

        public ZlinkStreamTypedSequenceBuilder<TPayload> Timeout(TimeSpan timeout)
        {
            _inner.Timeout(timeout);
            return this;
        }

        public async ValueTask<IReadOnlyList<ZlinkStreamMessage<TPayload>>> Async(
            CancellationToken cancellationToken = default
        )
        {
            var messages = await _inner.Async(cancellationToken);
            var decoded = new List<ZlinkStreamMessage<TPayload>>(messages.Count);
            foreach (var message in messages)
                decoded.Add(Decode(message));
            return decoded;
        }

        private ZlinkStreamMessage<TPayload> Decode(
            ZlinkStreamMessage<ZlinkStreamEncodedPayload> message
        )
        {
            return new ZlinkStreamMessage<TPayload>(
                message.Name,
                message.Metadata,
                ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(
                    _codec,
                    message.Payload
                ),
                message.ActorId
            );
        }
    }

    public sealed class ZlinkStreamTypedWaitBuilder<TPayload>
    {
        private readonly IZlinkStreamPayloadCodec _codec;
        private readonly IZlinkStreamWaitCall _inner;

        internal ZlinkStreamTypedWaitBuilder(
            IZlinkStreamWaitCall inner,
            IZlinkStreamPayloadCodec codec
        )
        {
            _inner = inner;
            _codec = codec;
        }

        public ZlinkStreamTypedWaitBuilder<TPayload> Timeout(TimeSpan timeout)
        {
            _inner.Timeout(timeout);
            return this;
        }

        public ZlinkStreamTypedWaitBuilder<TPayload> Where(
            Func<ZlinkStreamMessage<TPayload>, bool> predicate
        )
        {
            if (predicate is null)
                throw new ArgumentNullException(nameof(predicate));
            _inner.Where(message =>
                predicate(
                    new ZlinkStreamMessage<TPayload>(
                        message.Name,
                        message.Metadata,
                        ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(
                            _codec,
                            message.Payload
                        ),
                        message.ActorId
                    )
                )
            );
            return this;
        }

        public async ValueTask<ZlinkStreamMessage<TPayload>> Async(
            CancellationToken cancellationToken = default
        )
        {
            var message = await _inner.Async(cancellationToken);
            return new ZlinkStreamMessage<TPayload>(
                message.Name,
                message.Metadata,
                ZlinkStreamTypedConnectorExtensions.DecodePayload<TPayload>(
                    _codec,
                    message.Payload
                ),
                message.ActorId
            );
        }
    }

    public sealed class ZlinkStreamTypedSendBuilder
    {
        private readonly IZlinkStreamSendCall _inner;

        internal ZlinkStreamTypedSendBuilder(IZlinkStreamSendCall inner)
        {
            _inner = inner;
        }

        public ZlinkStreamTypedSendBuilder PacketName(string name)
        {
            _inner.PacketName(name);
            return this;
        }

        public ZlinkStreamTypedSendBuilder Metadata(string key, string value)
        {
            _inner.Metadata(key, value);
            return this;
        }

        public ZlinkStreamTypedSendBuilder Metadata(ZlinkStreamMetadata metadata)
        {
            _inner.Metadata(metadata);
            return this;
        }

        public ZlinkStreamTypedSendBuilder Compress()
        {
            _inner.Compress();
            return this;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            return _inner.Async(cancellationToken);
        }
    }

    public sealed class ZlinkStreamTypedRequestBuilder
    {
        private readonly IZlinkStreamPayloadCodec _codec;
        private readonly IZlinkStreamRequestCall _inner;

        internal ZlinkStreamTypedRequestBuilder(
            IZlinkStreamRequestCall inner,
            IZlinkStreamPayloadCodec codec
        )
        {
            _inner = inner;
            _codec = codec;
        }

        public ZlinkStreamTypedRequestBuilder PacketName(string name)
        {
            _inner.PacketName(name);
            return this;
        }

        public ZlinkStreamTypedRequestBuilder Metadata(string key, string value)
        {
            _inner.Metadata(key, value);
            return this;
        }

        public ZlinkStreamTypedRequestBuilder Metadata(ZlinkStreamMetadata metadata)
        {
            _inner.Metadata(metadata);
            return this;
        }

        public ZlinkStreamTypedRequestBuilder Timeout(TimeSpan timeout)
        {
            _inner.Timeout(timeout);
            return this;
        }

        public ZlinkStreamTypedRequestBuilder Compress()
        {
            _inner.Compress();
            return this;
        }

        public async ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
        {
            var reply = await _inner.Async(cancellationToken);
            return ZlinkStreamTypedConnectorExtensions.DecodePayload<TReply>(_codec, reply);
        }

        public void Submit(Action<ZlinkStreamResult> callback)
        {
            _inner.Submit(callback);
        }

        public void Submit<TReply>(Action<ZlinkStreamResult<TReply>> callback)
        {
            if (callback is null)
                throw new ArgumentNullException(nameof(callback));
            _inner.Submit(result =>
            {
                if (!result.IsSuccess)
                {
                    callback(ZlinkStreamResult<TReply>.Failure(result.Error));
                    return;
                }

                try
                {
                    callback(
                        ZlinkStreamResult<TReply>.Success(
                            ZlinkStreamTypedConnectorExtensions.DecodePayload<TReply>(
                                _codec,
                                result.Value
                            )
                        )
                    );
                }
                catch (Exception exception)
                {
                    callback(
                        ZlinkStreamResult<TReply>.Failure(
                            new ZlinkStreamError(
                                ZlinkStreamErrorCode.UserCallbackFailed,
                                "Stream reply decode failed.",
                                exception
                            )
                        )
                    );
                }
            });
        }
    }
}
