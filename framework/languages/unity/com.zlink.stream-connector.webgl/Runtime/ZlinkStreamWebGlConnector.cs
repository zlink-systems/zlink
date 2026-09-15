using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;
using Systems.Zlink.Stream.Connector.Runtime.Calls;

namespace Systems.Zlink.Stream.Connector.Runtime
{
    /// <summary>
    ///     WebGL implementation of <see cref="IZlinkStreamConnector" />. Owns no wire
    ///     behaviour: every frame, timer and reconnect decision belongs to the TypeScript
    ///     connector behind the jslib boundary.
    /// </summary>
    /// <remarks>
    ///     <para><b>How JavaScript reaches C#.</b></para>
    ///     <para>
    ///         The jslib side never calls a managed function from a WebSocket event or a
    ///         promise continuation. It appends to a queue. C# drains that queue by calling
    ///         <c>ZlinkStreamPump</c>, and <see cref="OnEvent" /> - the one managed function
    ///         JavaScript ever calls - only copies the event into <see cref="_inbox" />. It
    ///         runs no user code, awaits nothing and lets no exception escape into the
    ///         emscripten frame below it.
    ///     </para>
    ///     <para>
    ///         User callbacks run in <see cref="RunDispatchQueueAsync" />, after the jslib
    ///         call has returned and the JavaScript stack is unwound. A callback that calls
    ///         back into the connector therefore starts a new boundary call instead of
    ///         re-entering one that is still on the stack. The jslib pump additionally
    ///         refuses a nested drain, and a boundary call completes by setting a flag that
    ///         the frame loop reads - never by resuming an <c>await</c> from inside the
    ///         sink.
    ///     </para>
    /// </remarks>
    internal sealed class ZlinkStreamWebGlConnector : IZlinkStreamConnector
    {
        private const int MaxEventsPerPump = 256;

        private static readonly Dictionary<int, ZlinkStreamWebGlConnector> Instances =
            new Dictionary<int, ZlinkStreamWebGlConnector>();

        // Rooted for the lifetime of the domain: the function pointer handed to
        // JavaScript must stay valid while any connector exists.
        private static readonly ZlinkStreamInterop.EventCallback SinkDelegate = OnEvent;

        private readonly Queue<ZlinkStreamInboundEvent> _inbox = new Queue<ZlinkStreamInboundEvent>();
        private readonly Queue<DispatchItem> _dispatchQueue = new Queue<DispatchItem>();
        private readonly Dictionary<int, PendingCall> _pending = new Dictionary<int, PendingCall>();
        private readonly Dictionary<int, string> _observerNames = new Dictionary<int, string>();
        private readonly HashSet<string> _observed = new HashSet<string>(StringComparer.Ordinal);
        private readonly Dictionary<string, List<HandlerRegistration>> _handlers =
            new Dictionary<string, List<HandlerRegistration>>(StringComparer.Ordinal);
        private readonly ZlinkStreamReceivedMessages _received = new ZlinkStreamReceivedMessages();

        private readonly int _handle;
        private int _nextCallId = 1;
        private Task _advance;
        private ZlinkStreamConnectionState _state = ZlinkStreamConnectionState.Created;
        private ZlinkStreamCloseReason? _closeReason;
        private bool _disposed;

        internal ZlinkStreamWebGlConnector(ZlinkStreamConnectorOptions options)
        {
            if (options is null) throw new ArgumentNullException(nameof(options));
            Options = options;
            ValidateOptions(options);

            _handle = ZlinkStreamInterop.Create(BuildOptionsJson(options));
            if (_handle == 0) throw CreateFailure();

            Instances[_handle] = this;
            if (ZlinkStreamInterop.SetEventSink(_handle, Marshal.GetFunctionPointerForDelegate(SinkDelegate)) == 0)
            {
                Instances.Remove(_handle);
                ZlinkStreamInterop.Destroy(_handle);
                throw Error(ZlinkStreamErrorCode.ConfigurationError,
                    "The WebGL build could not install the ZLink stream event sink.");
            }

            Connect = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Connect);
            Close = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Close);
            Dispatch = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Dispatch);
        }

        public event Func<ZlinkStreamError, CancellationToken, ValueTask> ErrorReceived;

        public event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> Disconnected;

        public event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> ConnectionStateChanged;

        public bool IsConnected => !_disposed && ZlinkStreamInterop.IsConnected(_handle) != 0;

        public ZlinkStreamConnectionState State => _disposed ? ZlinkStreamConnectionState.Closed : _state;

        public ZlinkStreamConnectorOptions Options { get; }

        public ZlinkStreamDiagnosticsLevel DiagnosticsLevel => Options.DiagnosticsLevel;

        /// <summary>Callbacks waiting for the next <see cref="Dispatch" />.</summary>
        public int PendingDispatchCount => _dispatchQueue.Count;

        public IZlinkStreamLifecycleCall Connect { get; }

        public IZlinkStreamLifecycleCall Close { get; }

        public IZlinkStreamLifecycleCall Dispatch { get; }

        internal ZlinkStreamCloseReason? CloseReason => _closeReason;

        public void SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel level)
        {
            ThrowIfDisposed();
            if (ZlinkStreamInterop.SetDiagnosticsLevel(_handle, (int)level) == 0) throw CreateFailure();
            Options.SetDiagnosticsLevelLive(level);
        }

        public Task SetDiagnosticsLevelAsync(ZlinkStreamDiagnosticsLevel level)
        {
            SetDiagnosticsLevel(level);
            return Task.CompletedTask;
        }

        public int ReceivedCount(string name)
        {
            EnsureObserved(name);
            return _received.Count(name);
        }

        public IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload)
        {
            ThrowIfDisposed();
            if (payload is null) throw new ArgumentNullException(nameof(payload));
            return new ZlinkStreamSendBuilder(this, payload);
        }

        public IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload)
        {
            ThrowIfDisposed();
            if (payload is null) throw new ArgumentNullException(nameof(payload));
            return new ZlinkStreamRequestBuilder(this, payload);
        }

        public IDisposable On(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentNullException(nameof(name));
            if (handler is null) throw new ArgumentNullException(nameof(handler));
            ThrowIfDisposed();
            EnsureObserved(name);
            var registration = new HandlerRegistration(this, name, handler);
            if (!_handlers.TryGetValue(name, out var handlers))
            {
                handlers = new List<HandlerRegistration>();
                _handlers[name] = handlers;
            }

            handlers.Add(registration);
            return registration;
        }

        public IZlinkStreamWaitCall WaitFor(string name)
        {
            ThrowIfDisposed();
            EnsureObserved(name);
            return new ZlinkStreamWaitBuilder(this, name);
        }

        public IZlinkStreamExpectNoneCall ExpectNone(string name)
        {
            ThrowIfDisposed();
            EnsureObserved(name);
            return new ZlinkStreamExpectNoneBuilder(this, name);
        }

        public IZlinkStreamSequenceCall WaitForSequence(string name)
        {
            ThrowIfDisposed();
            EnsureObserved(name);
            return new ZlinkStreamSequenceBuilder(this, name);
        }

        public async ValueTask DisposeAsync()
        {
            if (_disposed) return;
            try
            {
                await Close.Async().ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Disposal must not throw over a connection that is already gone.
            }

            _disposed = true;
            Instances.Remove(_handle);
            ZlinkStreamInterop.Destroy(_handle);
        }

        internal static ZlinkStreamException Error(ZlinkStreamErrorCode code, string message)
        {
            return new ZlinkStreamException(new ZlinkStreamError(code, message));
        }

        // ------------------------------------------------------------------
        // Lifecycle calls
        // ------------------------------------------------------------------

        internal ValueTask RunLifecycleAsync(ZlinkStreamLifecycleKind kind, CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            switch (kind)
            {
                case ZlinkStreamLifecycleKind.Connect:
                    return new ValueTask(RunConnectAsync(cancellationToken));
                case ZlinkStreamLifecycleKind.Close:
                    return new ValueTask(RunCloseAsync(cancellationToken));
                default:
                    return new ValueTask(RunDispatchAsync(cancellationToken));
            }
        }

        private async Task RunConnectAsync(CancellationToken cancellationToken)
        {
            var callId = NextCallId();
            var pending = RegisterPending(callId);
            ZlinkStreamInterop.Connect(_handle, callId);
            await DriveAsync(callId, pending, false, cancellationToken).ConfigureAwait(false);
        }

        private async Task RunCloseAsync(CancellationToken cancellationToken)
        {
            var callId = NextCallId();
            var pending = RegisterPending(callId);
            ZlinkStreamInterop.Close(_handle, callId);
            await DriveAsync(callId, pending, false, cancellationToken).ConfigureAwait(false);
        }

        private async Task RunDispatchAsync(CancellationToken cancellationToken)
        {
            // Reads whatever the transport has, then runs the callbacks that were waiting
            // for it. Only this path runs registered handlers, which is what lets the wait
            // surfaces observe the unread queue without a dispatch pump
            // (stream-connector spec 32 section 7).
            StartAdvanceIfIdle();
            var advance = _advance;
            if (advance != null) await advance.ConfigureAwait(false);
            PumpAndTransfer();
            await RunDispatchQueueAsync(cancellationToken).ConfigureAwait(false);
        }

        internal async ValueTask SendAsync(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            CancellationToken cancellationToken)
        {
            var call = new ZlinkStreamJson.Writer().StartObject();
            call.Number("codec", (int)payload.Codec);
            if (packetName != null) call.String("packetName", packetName);
            WriteMetadata(call, metadata);
            call.Bool("compress", compress);
            call.EndObject();

            var callId = NextCallId();
            var pending = RegisterPending(callId);
            InvokeWithPayload(payload, (pointer, length) =>
                ZlinkStreamInterop.Send(_handle, callId, call.ToString(), pointer, length));
            await DriveAsync(callId, pending, true, cancellationToken).ConfigureAwait(false);
        }

        internal async ValueTask<ZlinkStreamEncodedPayload> RequestAsync(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            CancellationToken cancellationToken)
        {
            var callId = StartRequest(payload, packetName, metadata, compress, timeout, out var pending);
            await DriveAsync(callId, pending, true, cancellationToken).ConfigureAwait(false);
            return pending.Result;
        }

        internal void SubmitRequest(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback)
        {
            var callId = StartRequest(payload, packetName, metadata, compress, timeout, out var pending);
            // Spec 32 section 7: request callbacks run on Dispatch, like push handlers.
            pending.Callback = callback;
            _ = DriveQuietlyAsync(callId, pending);
        }

        private int StartRequest(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            out PendingCall pending)
        {
            ThrowIfDisposed();
            var call = new ZlinkStreamJson.Writer().StartObject();
            call.Number("codec", (int)payload.Codec);
            if (packetName != null) call.String("packetName", packetName);
            WriteMetadata(call, metadata);
            call.Bool("compress", compress);
            call.Number("timeoutMs", (timeout ?? Options.RequestTimeout).TotalMilliseconds);
            call.EndObject();

            var callId = NextCallId();
            pending = RegisterPending(callId);
            var json = call.ToString();
            InvokeWithPayload(payload, (pointer, length) =>
                ZlinkStreamInterop.Request(_handle, callId, json, pointer, length));
            return callId;
        }

        private async Task DriveQuietlyAsync(int callId, PendingCall pending)
        {
            try
            {
                await DriveAsync(callId, pending, true, CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // The failure was already delivered to the caller's callback.
            }
        }

        // ------------------------------------------------------------------
        // Pump
        // ------------------------------------------------------------------

        /// <summary>
        ///     Runs the frame loop for one outstanding boundary call: pump the JavaScript
        ///     event queue, move the events into managed structures, yield to the player
        ///     loop so the browser can make progress, repeat.
        /// </summary>
        /// <remarks>
        ///     <c>Task.Yield</c> posts the continuation to Unity's synchronization context,
        ///     so the loop advances once per frame on the main thread and never blocks the
        ///     single WebGL thread.
        /// </remarks>
        private async Task DriveAsync(
            int callId,
            PendingCall pending,
            bool advanceTransport,
            CancellationToken cancellationToken)
        {
            try
            {
                while (!pending.Completed)
                {
                    if (advanceTransport && IsConnected) StartAdvanceIfIdle();
                    PumpAndTransfer();
                    if (pending.Completed) break;
                    if (cancellationToken.IsCancellationRequested)
                    {
                        ZlinkStreamInterop.Cancel(_handle, callId);
                        cancellationToken.ThrowIfCancellationRequested();
                    }

                    await Task.Yield();
                }

                if (pending.Error != null) throw new ZlinkStreamException(pending.Error);
            }
            finally
            {
                _pending.Remove(callId);
            }
        }

        private void StartAdvanceIfIdle()
        {
            if (_advance != null && !_advance.IsCompleted) return;
            var callId = NextCallId();
            var pending = RegisterPending(callId);
            ZlinkStreamInterop.Dispatch(_handle, callId);
            _advance = DriveAsync(callId, pending, false, CancellationToken.None);
            _advance.ContinueWith(
                static task => { _ = task.Exception; },
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously);
        }

        /// <summary>
        ///     Moves queued JavaScript events into managed state. Safe to call anywhere: it
        ///     runs no user code.
        /// </summary>
        private void PumpAndTransfer()
        {
            if (_disposed) return;
            var drained = ZlinkStreamInterop.Pump(_handle, MaxEventsPerPump);
            // A refusal means an outer pump on this stack is draining the same queue,
            // so there is nothing to do. A failure means the boundary stopped
            // delivering, and staying quiet about it would leave every caller waiting
            // for events that are no longer coming.
            if (drained == ZlinkStreamInterop.PumpFailed) throw BoundaryFailure();
            while (_inbox.Count > 0) Transfer(_inbox.Dequeue());
        }

        /// <summary>
        ///     The boundary itself broke. That is not one of the stream errors in
        ///     stream-connector spec 32 section 9 - which is why this is not a
        ///     <see cref="ZlinkStreamException" /> with an invented code - so it surfaces
        ///     the way a broken object does, out of whichever call was pumping.
        /// </summary>
        private static InvalidOperationException BoundaryFailure()
        {
            var text = ZlinkStreamInterop.TakeLastErrorText();
            return new InvalidOperationException(
                string.IsNullOrEmpty(text)
                    ? "The ZLink WebGL stream boundary failed while draining events."
                    : "The ZLink WebGL stream boundary failed while draining events: " + text);
        }

        private void Transfer(ZlinkStreamInboundEvent inbound)
        {
            switch (inbound.EventType)
            {
                case ZlinkStreamInterop.EventCallCompleted:
                    CompleteCall(inbound);
                    break;
                case ZlinkStreamInterop.EventMessage:
                    RouteMessage(inbound);
                    break;
                case ZlinkStreamInterop.EventErrorReceived:
                    _dispatchQueue.Enqueue(DispatchItem.ForError(ParseError(inbound.Text)));
                    break;
                case ZlinkStreamInterop.EventDisconnected:
                    _closeReason = ParseCloseReason(inbound.Text);
                    _dispatchQueue.Enqueue(DispatchItem.ForDisconnected(
                        new ZlinkStreamDisconnected(_closeReason ?? ZlinkStreamCloseReason.TransportError)));
                    break;
                case ZlinkStreamInterop.EventStateChanged:
                    var change = ParseStateChange(inbound.Text);
                    _state = change.Current;
                    _dispatchQueue.Enqueue(DispatchItem.ForStateChange(change));
                    break;
            }
        }

        private void CompleteCall(ZlinkStreamInboundEvent inbound)
        {
            if (!_pending.TryGetValue(inbound.Id, out var pending)) return;
            if (inbound.Value == 1)
            {
                ZlinkStreamEncodedPayload payload = null;
                if (inbound.Bytes != null)
                {
                    var codec = (ZlinkStreamCodec)ZlinkStreamJson.Parse(inbound.Text).IntOf("codec", 0);
                    payload = new ZlinkStreamEncodedPayload(codec, inbound.Bytes);
                }

                pending.Complete(payload);
                if (pending.Callback != null)
                {
                    _dispatchQueue.Enqueue(DispatchItem.ForRequestResult(
                        pending.Callback,
                        ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
                            payload ?? new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, ReadOnlyMemory<byte>.Empty))));
                }

                return;
            }

            var error = ParseError(inbound.Text);
            pending.Fail(error);
            if (pending.Callback != null)
            {
                _dispatchQueue.Enqueue(DispatchItem.ForRequestResult(
                    pending.Callback,
                    ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure(error)));
            }
        }

        private void RouteMessage(ZlinkStreamInboundEvent inbound)
        {
            if (!_observerNames.TryGetValue(inbound.Id, out var observedName)) return;
            var node = ZlinkStreamJson.Parse(inbound.Text);
            var name = node.TextOf("name") ?? observedName;
            var metadata = ReadMetadata(node.Member("metadata"));
            var payload = new ZlinkStreamEncodedPayload(
                (ZlinkStreamCodec)inbound.Value,
                inbound.Bytes ?? ReadOnlyMemory<byte>.Empty);
            var message = new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(name, metadata, payload);

            // Same rule as the native connector: a registered handler takes the message,
            // otherwise it waits in the unread history for a wait surface.
            if (_handlers.TryGetValue(name, out var handlers) && handlers.Count > 0)
            {
                _dispatchQueue.Enqueue(DispatchItem.ForMessage(handlers.ToArray(), message));
                return;
            }

            _received.Record(message);
        }

        /// <summary>
        ///     Runs queued user callbacks. Called only after a jslib call has returned, so
        ///     no user code ever runs on a JavaScript stack.
        /// </summary>
        private async Task RunDispatchQueueAsync(CancellationToken cancellationToken)
        {
            while (_dispatchQueue.Count > 0)
            {
                var item = _dispatchQueue.Dequeue();
                try
                {
                    await item.InvokeAsync(this, cancellationToken).ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    var handler = ErrorReceived;
                    if (handler is null) continue;
                    var error = new ZlinkStreamError(
                        ZlinkStreamErrorCode.UserCallbackFailed,
                        "Stream callback failed.",
                        exception);
                    try
                    {
                        await handler(error, cancellationToken).ConfigureAwait(false);
                    }
                    catch (Exception)
                    {
                        // A failing error handler must not abort the dispatch pass.
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // Wait surfaces
        // ------------------------------------------------------------------

        internal async ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForEncodedAsync(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate,
            TimeSpan timeout,
            CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            EnsureObserved(name);
            var elapsed = Stopwatch.StartNew();
            while (true)
            {
                if (IsConnected) StartAdvanceIfIdle();
                PumpAndTransfer();
                var message = _received.TryTake(name, predicate);
                if (message != null) return message;
                cancellationToken.ThrowIfCancellationRequested();
                if (elapsed.Elapsed >= timeout)
                    throw new TimeoutException($"Timed out waiting for '{name}' stream message.");
                await Task.Yield();
            }
        }

        internal void EnsureObserved(string name)
        {
            if (string.IsNullOrEmpty(name)) throw new ArgumentNullException(nameof(name));
            if (_disposed || _observed.Contains(name)) return;
            var observerId = ZlinkStreamInterop.Observe(_handle, name);
            if (observerId == 0) return;
            _observed.Add(name);
            _observerNames[observerId] = name;
        }

        internal void RemoveHandler(string name, HandlerRegistration registration)
        {
            if (!_handlers.TryGetValue(name, out var handlers)) return;
            handlers.Remove(registration);
            if (handlers.Count == 0) _handlers.Remove(name);
        }

        // ------------------------------------------------------------------
        // Boundary sink
        // ------------------------------------------------------------------

#if UNITY_5_3_OR_NEWER
        [AOT.MonoPInvokeCallback(typeof(ZlinkStreamInterop.EventCallback))]
#endif
        private static void OnEvent(
            int handle,
            int eventType,
            int id,
            int value,
            IntPtr text,
            IntPtr bytes,
            int bytesLength)
        {
            // Runs on the JavaScript stack, inside ZlinkStreamPump. It copies and returns:
            // no user code, no await, no exception. The text and bytes pointers are freed
            // by the caller as soon as this returns, so both are copied here.
            try
            {
                if (!Instances.TryGetValue(handle, out var connector)) return;
                var copiedText = text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text);
                byte[] copiedBytes = null;
                if (bytes != IntPtr.Zero && bytesLength > 0)
                {
                    copiedBytes = new byte[bytesLength];
                    Marshal.Copy(bytes, copiedBytes, 0, bytesLength);
                }

                connector._inbox.Enqueue(new ZlinkStreamInboundEvent(eventType, id, value, copiedText, copiedBytes));
            }
            catch (Exception)
            {
                // An exception must never unwind into the emscripten frame below.
            }
        }

        // ------------------------------------------------------------------
        // Helpers
        // ------------------------------------------------------------------

        private int NextCallId()
        {
            return _nextCallId++;
        }

        private PendingCall RegisterPending(int callId)
        {
            var pending = new PendingCall();
            _pending[callId] = pending;
            return pending;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(IZlinkStreamConnector));
        }

        private static void InvokeWithPayload(ZlinkStreamEncodedPayload payload, Action<IntPtr, int> call)
        {
            // The pointer is owned here: JavaScript copies the bytes synchronously and the
            // buffer is released before this method returns.
            var length = payload.Payload.Length;
            if (length == 0)
            {
                call(IntPtr.Zero, 0);
                return;
            }

            var buffer = Marshal.AllocHGlobal(length);
            try
            {
                Marshal.Copy(payload.Payload.ToArray(), 0, buffer, length);
                call(buffer, length);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        private static void WriteMetadata(ZlinkStreamJson.Writer writer, ZlinkStreamMetadata metadata)
        {
            if (metadata is null || metadata.Count == 0) return;
            writer.Name("metadata").StartObject();
            foreach (var pair in metadata.Values) writer.String(pair.Key, pair.Value);
            writer.EndObject();
        }

        private static ZlinkStreamMetadata ReadMetadata(ZlinkStreamJson.Node node)
        {
            if (node is null) return ZlinkStreamMetadata.Empty;
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var pair in node.Members())
            {
                if (pair.Value?.Text != null) values[pair.Key] = pair.Value.Text;
            }

            return ZlinkStreamMetadata.FromDictionary(values);
        }

        private static ZlinkStreamError ParseError(string json)
        {
            var node = ZlinkStreamJson.Parse(json);
            return new ZlinkStreamError(
                ParseErrorCode(node.TextOf("code")),
                node.TextOf("message") ?? "Stream operation failed.");
        }

        private static ZlinkStreamErrorCode ParseErrorCode(string code)
        {
            switch (code)
            {
                case "disconnected": return ZlinkStreamErrorCode.Disconnected;
                case "configurationError": return ZlinkStreamErrorCode.ConfigurationError;
                case "validationFailed": return ZlinkStreamErrorCode.ValidationFailed;
                case "requestTimeout": return ZlinkStreamErrorCode.RequestTimeout;
                case "connectTimeout": return ZlinkStreamErrorCode.ConnectTimeout;
                case "frameDecodeFailed": return ZlinkStreamErrorCode.FrameDecodeFailed;
                case "frameTooLarge": return ZlinkStreamErrorCode.FrameTooLarge;
                case "compressionFailed": return ZlinkStreamErrorCode.CompressionFailed;
                case "decompressionFailed": return ZlinkStreamErrorCode.DecompressionFailed;
                case "tlsValidationFailed": return ZlinkStreamErrorCode.TlsValidationFailed;
                case "userCallbackFailed": return ZlinkStreamErrorCode.UserCallbackFailed;
                case "remoteError": return ZlinkStreamErrorCode.RemoteError;
                default: return ZlinkStreamErrorCode.SendFailed;
            }
        }

        private static ZlinkStreamCloseReason? ParseCloseReason(string json)
        {
            switch (ZlinkStreamJson.Parse(json).TextOf("closeReason"))
            {
                case "ClientClose": return ZlinkStreamCloseReason.ClientClose;
                case "IdleTimeout": return ZlinkStreamCloseReason.IdleTimeout;
                case "HeartbeatTimeout": return ZlinkStreamCloseReason.HeartbeatTimeout;
                case "ServerDrain": return ZlinkStreamCloseReason.ServerDrain;
                case "ProtocolError": return ZlinkStreamCloseReason.ProtocolError;
                case "TransportError": return ZlinkStreamCloseReason.TransportError;
                default: return null;
            }
        }

        private static ZlinkStreamConnectionStateChanged ParseStateChange(string json)
        {
            var node = ZlinkStreamJson.Parse(json);
            var errorNode = node.Member("error");
            ZlinkStreamError error = null;
            if (errorNode?.TextOf("code") != null)
            {
                error = new ZlinkStreamError(
                    ParseErrorCode(errorNode.TextOf("code")),
                    errorNode.TextOf("message") ?? string.Empty);
            }

            return new ZlinkStreamConnectionStateChanged(
                ParseState(node.TextOf("previous")),
                ParseState(node.TextOf("current")),
                error);
        }

        private static ZlinkStreamConnectionState ParseState(string state)
        {
            switch (state)
            {
                case "connecting": return ZlinkStreamConnectionState.Connecting;
                case "connected": return ZlinkStreamConnectionState.Connected;
                case "reconnecting": return ZlinkStreamConnectionState.Reconnecting;
                case "disconnected": return ZlinkStreamConnectionState.Disconnected;
                case "closed": return ZlinkStreamConnectionState.Closed;
                default: return ZlinkStreamConnectionState.Created;
            }
        }

        private static void ValidateOptions(ZlinkStreamConnectorOptions options)
        {
            if (options.Endpoint is null)
                throw Error(ZlinkStreamErrorCode.ConfigurationError, "Endpoint is required.");
            if (options.CompressionCodec != null)
                throw Error(ZlinkStreamErrorCode.ConfigurationError,
                    "CompressionCodec is not supported on WebGL: the compression codec runs in the " +
                    "JavaScript connector. Select the algorithm with Compression instead.");
            if (options.SkipServerCertificateValidation)
                throw Error(ZlinkStreamErrorCode.ConfigurationError,
                    "SkipServerCertificateValidation is not supported on WebGL: the browser owns " +
                    "certificate validation for wss:// and offers no way to skip it.");
        }

        private static string BuildOptionsJson(ZlinkStreamConnectorOptions options)
        {
            var writer = new ZlinkStreamJson.Writer().StartObject();
            writer.String("endpoint", options.Endpoint.ToString());
            if (options.Transport.HasValue)
            {
                writer.String("transport",
                    options.Transport.Value == ZlinkStreamTransport.WebSocketSecure ? "webSocketSecure" : "webSocket");
            }

            writer.Number("connectTimeoutMs", options.ConnectTimeout.TotalMilliseconds);
            writer.Number("requestTimeoutMs", options.RequestTimeout.TotalMilliseconds);
            writer.Number("waitTimeoutMs", options.WaitTimeout.TotalMilliseconds);
            writer.Name("heartbeat").StartObject();
            writer.Bool("enabled", options.Heartbeat.Enabled);
            writer.Number("intervalMs", options.Heartbeat.Interval.TotalMilliseconds);
            writer.Number("timeoutMs", options.Heartbeat.Timeout.TotalMilliseconds);
            writer.EndObject();
            writer.Name("reconnect").StartObject();
            writer.Bool("enabled", options.Reconnect.Enabled);
            writer.Number("initialDelayMs", options.Reconnect.InitialDelay.TotalMilliseconds);
            writer.Number("maxDelayMs", options.Reconnect.MaxDelay.TotalMilliseconds);
            writer.Number("backoffFactor", options.Reconnect.BackoffFactor);
            writer.Number("maxAttempts", options.Reconnect.MaxAttempts ?? 3);
            writer.EndObject();
            writer.Number("maxSendPayloadSize", options.MaxSendPayloadSize);
            writer.Number("maxReceivePayloadSize", options.MaxReceivePayloadSize);
            writer.String("dispatchMode",
                options.DispatchMode == ZlinkStreamDispatchMode.Immediate ? "immediate" : "manual");
            writer.String("compression", options.Compression == ZlinkStreamCompression.None ? "none" : "lz4");
            writer.String("diagnosticsLevel", DiagnosticsName(options.DiagnosticsLevel));
            writer.EndObject();
            return writer.ToString();
        }

        private static string DiagnosticsName(ZlinkStreamDiagnosticsLevel level)
        {
            switch (level)
            {
                case ZlinkStreamDiagnosticsLevel.Off: return "off";
                case ZlinkStreamDiagnosticsLevel.Normal: return "normal";
                case ZlinkStreamDiagnosticsLevel.Detailed: return "detailed";
                default: return "errors";
            }
        }

        private static ZlinkStreamException CreateFailure()
        {
            var text = ZlinkStreamInterop.TakeLastErrorText();
            if (string.IsNullOrEmpty(text))
            {
                return Error(ZlinkStreamErrorCode.ConfigurationError, "The stream connector could not be created.");
            }

            return new ZlinkStreamException(ParseError(text));
        }

        internal sealed class HandlerRegistration : IDisposable
        {
            private readonly ZlinkStreamWebGlConnector _connector;
            private readonly string _name;

            internal HandlerRegistration(
                ZlinkStreamWebGlConnector connector,
                string name,
                Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
            {
                _connector = connector;
                _name = name;
                Handler = handler;
            }

            internal Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> Handler { get; }

            public void Dispose()
            {
                _connector.RemoveHandler(_name, this);
            }
        }

        private sealed class PendingCall
        {
            public bool Completed { get; private set; }

            public ZlinkStreamEncodedPayload Result { get; private set; }

            public ZlinkStreamError Error { get; private set; }

            public Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> Callback { get; set; }

            public void Complete(ZlinkStreamEncodedPayload result)
            {
                Result = result;
                Completed = true;
            }

            public void Fail(ZlinkStreamError error)
            {
                Error = error;
                Completed = true;
            }
        }

        private readonly struct ZlinkStreamInboundEvent
        {
            public ZlinkStreamInboundEvent(int eventType, int id, int value, string text, byte[] bytes)
            {
                EventType = eventType;
                Id = id;
                Value = value;
                Text = text;
                Bytes = bytes;
            }

            public int EventType { get; }

            public int Id { get; }

            public int Value { get; }

            public string Text { get; }

            public byte[] Bytes { get; }
        }

        private readonly struct DispatchItem
        {
            private readonly int _kind;
            private readonly HandlerRegistration[] _handlers;
            private readonly ZlinkStreamMessage<ZlinkStreamEncodedPayload> _message;
            private readonly ZlinkStreamError _error;
            private readonly ZlinkStreamDisconnected _disconnected;
            private readonly ZlinkStreamConnectionStateChanged _stateChange;
            private readonly Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> _callback;
            private readonly ZlinkStreamResult<ZlinkStreamEncodedPayload> _result;

            private DispatchItem(
                int kind,
                HandlerRegistration[] handlers,
                ZlinkStreamMessage<ZlinkStreamEncodedPayload> message,
                ZlinkStreamError error,
                ZlinkStreamDisconnected disconnected,
                ZlinkStreamConnectionStateChanged stateChange,
                Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback,
                ZlinkStreamResult<ZlinkStreamEncodedPayload> result)
            {
                _kind = kind;
                _handlers = handlers;
                _message = message;
                _error = error;
                _disconnected = disconnected;
                _stateChange = stateChange;
                _callback = callback;
                _result = result;
            }

            public static DispatchItem ForMessage(
                HandlerRegistration[] handlers,
                ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
            {
                return new DispatchItem(1, handlers, message, null, null, null, null, default);
            }

            public static DispatchItem ForError(ZlinkStreamError error)
            {
                return new DispatchItem(2, null, null, error, null, null, null, default);
            }

            public static DispatchItem ForDisconnected(ZlinkStreamDisconnected disconnected)
            {
                return new DispatchItem(3, null, null, null, disconnected, null, null, default);
            }

            public static DispatchItem ForStateChange(ZlinkStreamConnectionStateChanged change)
            {
                return new DispatchItem(4, null, null, null, null, change, null, default);
            }

            public static DispatchItem ForRequestResult(
                Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback,
                ZlinkStreamResult<ZlinkStreamEncodedPayload> result)
            {
                return new DispatchItem(5, null, null, null, null, null, callback, result);
            }

            public async ValueTask InvokeAsync(
                ZlinkStreamWebGlConnector connector,
                CancellationToken cancellationToken)
            {
                switch (_kind)
                {
                    case 1:
                        foreach (var handler in _handlers)
                            await handler.Handler(_message, cancellationToken).ConfigureAwait(false);
                        break;
                    case 2:
                    {
                        var handler = connector.ErrorReceived;
                        if (handler != null) await handler(_error, cancellationToken).ConfigureAwait(false);
                        break;
                    }

                    case 3:
                    {
                        var handler = connector.Disconnected;
                        if (handler != null) await handler(_disconnected, cancellationToken).ConfigureAwait(false);
                        break;
                    }

                    case 4:
                    {
                        var handler = connector.ConnectionStateChanged;
                        if (handler != null) await handler(_stateChange, cancellationToken).ConfigureAwait(false);
                        break;
                    }

                    default:
                        _callback(_result);
                        break;
                }
            }
        }
    }

    internal enum ZlinkStreamLifecycleKind
    {
        Connect,
        Close,
        Dispatch
    }
}
