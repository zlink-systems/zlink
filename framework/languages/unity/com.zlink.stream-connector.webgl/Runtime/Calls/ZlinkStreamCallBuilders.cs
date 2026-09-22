// No ConfigureAwait(false) in this package: a WebGL player has no thread pool, so a
// continuation that did not capture the context is queued and never runs. See the
// remarks on ZlinkStreamWebGlConnector.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;

namespace Systems.Zlink.Stream.Connector.Runtime.Calls
{
    internal sealed class ZlinkStreamLifecycleCall : IZlinkStreamLifecycleCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly ZlinkStreamLifecycleKind _kind;

        internal ZlinkStreamLifecycleCall(
            ZlinkStreamWebGlConnector connector,
            ZlinkStreamLifecycleKind kind
        )
        {
            _connector = connector;
            _kind = kind;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            return _connector.RunLifecycleAsync(_kind, cancellationToken);
        }
    }

    internal abstract class ZlinkStreamCallBuilder
    {
        private bool _executed;
        private Dictionary<string, string> _metadata;

        protected string PacketNameValue { get; private set; }

        protected bool CompressValue { get; private set; }

        protected ZlinkStreamMetadata MetadataValue
        {
            get
            {
                return _metadata is null
                    ? ZlinkStreamMetadata.Empty
                    : ZlinkStreamMetadata.FromDictionary(_metadata);
            }
        }

        protected void SetPacketName(string name)
        {
            if (string.IsNullOrEmpty(name))
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Packet name must not be empty."
                );
            PacketNameValue = name;
        }

        protected void AddMetadata(string key, string value)
        {
            if (string.IsNullOrEmpty(key))
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Metadata key must not be empty."
                );
            if (value is null)
                throw new ArgumentNullException(nameof(value));
            _metadata ??= new Dictionary<string, string>(StringComparer.Ordinal);
            _metadata[key] = value;
        }

        protected void AddMetadata(ZlinkStreamMetadata metadata)
        {
            if (metadata is null)
                throw new ArgumentNullException(nameof(metadata));
            foreach (var pair in metadata.Values)
                AddMetadata(pair.Key, pair.Value);
        }

        protected void SetCompress()
        {
            CompressValue = true;
        }

        protected void EnsureNotExecuted()
        {
            if (_executed)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Builder instances can be executed only once."
                );
            _executed = true;
        }
    }

    internal sealed class ZlinkStreamSendBuilder : ZlinkStreamCallBuilder, IZlinkStreamSendCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly ZlinkStreamEncodedPayload _payload;
        private readonly ZlinkStreamActor _actor;

        internal ZlinkStreamSendBuilder(
            ZlinkStreamWebGlConnector connector,
            ZlinkStreamEncodedPayload payload,
            ZlinkStreamActor actor = null
        )
        {
            _connector = connector;
            _payload = payload;
            _actor = actor;
        }

        public IZlinkStreamSendCall PacketName(string name)
        {
            SetPacketName(name);
            return this;
        }

        public IZlinkStreamSendCall Metadata(string key, string value)
        {
            AddMetadata(key, value);
            return this;
        }

        public IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata)
        {
            AddMetadata(metadata);
            return this;
        }

        public IZlinkStreamSendCall Compress()
        {
            SetCompress();
            return this;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            EnsureNotExecuted();
            _actor?.EnsureBound();
            return _connector.SendAsync(
                _payload,
                PacketNameValue,
                MetadataValue,
                CompressValue,
                cancellationToken,
                _actor
            );
        }
    }

    internal sealed class ZlinkStreamRequestBuilder
        : ZlinkStreamCallBuilder,
            IZlinkStreamRequestCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly ZlinkStreamEncodedPayload _payload;
        private readonly ZlinkStreamActor _actor;
        private TimeSpan? _timeout;

        internal ZlinkStreamRequestBuilder(
            ZlinkStreamWebGlConnector connector,
            ZlinkStreamEncodedPayload payload,
            ZlinkStreamActor actor = null
        )
        {
            _connector = connector;
            _payload = payload;
            _actor = actor;
        }

        public IZlinkStreamRequestCall PacketName(string name)
        {
            SetPacketName(name);
            return this;
        }

        public IZlinkStreamRequestCall Metadata(string key, string value)
        {
            AddMetadata(key, value);
            return this;
        }

        public IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata)
        {
            AddMetadata(metadata);
            return this;
        }

        public IZlinkStreamRequestCall Timeout(TimeSpan timeout)
        {
            if (timeout <= TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(
                    nameof(timeout),
                    "Request timeout must be greater than zero."
                );
            _timeout = timeout;
            return this;
        }

        public IZlinkStreamRequestCall Compress()
        {
            SetCompress();
            return this;
        }

        public ValueTask<ZlinkStreamEncodedPayload> Async(
            CancellationToken cancellationToken = default
        )
        {
            EnsureNotExecuted();
            _actor?.EnsureBound();
            return _connector.RequestAsync(
                _payload,
                PacketNameValue,
                MetadataValue,
                CompressValue,
                _timeout,
                cancellationToken,
                _actor
            );
        }

        public void Submit(Action<ZlinkStreamResult> callback)
        {
            if (callback is null)
                throw new ArgumentNullException(nameof(callback));
            Submit(
                (Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>>)(
                    result =>
                        callback(
                            result.IsSuccess
                                ? ZlinkStreamResult.Success()
                                : ZlinkStreamResult.Failure(result.Error)
                        )
                )
            );
        }

        public void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback)
        {
            if (callback is null)
                throw new ArgumentNullException(nameof(callback));
            EnsureNotExecuted();
            _actor?.EnsureBound();
            _connector.SubmitRequest(
                _payload,
                PacketNameValue,
                MetadataValue,
                CompressValue,
                _timeout,
                callback,
                _actor
            );
        }
    }

    internal sealed class ZlinkStreamWaitBuilder : IZlinkStreamWaitCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly string _name;
        private bool _executed;
        private Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> _predicate;
        private TimeSpan? _timeout;

        internal ZlinkStreamWaitBuilder(ZlinkStreamWebGlConnector connector, string name)
        {
            _connector = connector;
            _name = name;
        }

        public IZlinkStreamWaitCall Timeout(TimeSpan timeout)
        {
            if (timeout <= TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(
                    nameof(timeout),
                    "Wait timeout must be greater than zero."
                );
            _timeout = timeout;
            return this;
        }

        public IZlinkStreamWaitCall Where(
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate
        )
        {
            if (predicate is null)
                throw new ArgumentNullException(nameof(predicate));
            var previous = _predicate;
            _predicate = previous is null
                ? predicate
                : message => previous(message) && predicate(message);
            return this;
        }

        public ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(
            CancellationToken cancellationToken = default
        )
        {
            if (_executed)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Builder instances can be executed only once."
                );
            _executed = true;
            return _connector.WaitForEncodedAsync(
                _name,
                _predicate,
                _timeout ?? _connector.Options.WaitTimeout,
                cancellationToken
            );
        }
    }

    internal sealed class ZlinkStreamExpectNoneBuilder : IZlinkStreamExpectNoneCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly string _name;
        private bool _executed;
        private TimeSpan? _window;

        internal ZlinkStreamExpectNoneBuilder(ZlinkStreamWebGlConnector connector, string name)
        {
            _connector = connector;
            _name = name;
        }

        public IZlinkStreamExpectNoneCall Within(TimeSpan window)
        {
            if (window <= TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(
                    nameof(window),
                    "Observation window must be greater than zero."
                );
            _window = window;
            return this;
        }

        public async ValueTask Async(CancellationToken cancellationToken = default)
        {
            if (_executed)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Builder instances can be executed only once."
                );
            _executed = true;
            if (!_window.HasValue)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "ExpectNone requires Within(window)."
                );

            try
            {
                await _connector.WaitForEncodedAsync(_name, null, _window.Value, cancellationToken);
            }
            catch (TimeoutException)
            {
                return;
            }

            throw new InvalidOperationException(
                $"Expected no '{_name}' stream message within {_window.Value}."
            );
        }
    }

    internal sealed class ZlinkStreamSequenceBuilder : IZlinkStreamSequenceCall
    {
        private readonly ZlinkStreamWebGlConnector _connector;
        private readonly List<
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>
        > _expectations = new List<Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>>();

        private readonly string _name;
        private bool _executed;
        private TimeSpan? _timeout;

        internal ZlinkStreamSequenceBuilder(ZlinkStreamWebGlConnector connector, string name)
        {
            _connector = connector;
            _name = name;
        }

        public IZlinkStreamSequenceCall Expect(
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate
        )
        {
            if (predicate is null)
                throw new ArgumentNullException(nameof(predicate));
            _expectations.Add(predicate);
            return this;
        }

        public IZlinkStreamSequenceCall Timeout(TimeSpan timeout)
        {
            if (timeout <= TimeSpan.Zero)
                throw new ArgumentOutOfRangeException(
                    nameof(timeout),
                    "Sequence timeout must be greater than zero."
                );
            _timeout = timeout;
            return this;
        }

        public async ValueTask<IReadOnlyList<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> Async(
            CancellationToken cancellationToken = default
        )
        {
            if (_executed)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Builder instances can be executed only once."
                );
            _executed = true;
            if (_expectations.Count == 0)
                throw ZlinkStreamWebGlConnector.Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "WaitForSequence requires at least one expectation."
                );

            var timeout = _timeout ?? _connector.Options.WaitTimeout;
            var elapsed = Stopwatch.StartNew();
            var messages = new List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>(
                _expectations.Count
            );
            for (var index = 0; index < _expectations.Count; index++)
            {
                var remaining = timeout - elapsed.Elapsed;
                if (remaining <= TimeSpan.Zero)
                    throw new TimeoutException(
                        $"Timed out waiting for '{_name}' stream message sequence."
                    );

                var message = await _connector.WaitForEncodedAsync(
                    _name,
                    null,
                    remaining,
                    cancellationToken
                );
                if (!_expectations[index](message))
                    throw new InvalidOperationException(
                        $"Stream message '{_name}' arrived out of the expected sequence at index {index}."
                    );
                messages.Add(message);
            }

            return messages;
        }
    }
}
