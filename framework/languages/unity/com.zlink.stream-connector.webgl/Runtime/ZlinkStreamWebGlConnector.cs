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
    ///     <para><b>No <c>ConfigureAwait(false)</c> in this package.</b></para>
    ///     <para>
    ///         A WebGL player is single-threaded and has no thread pool, so a continuation
    ///         that did not capture the synchronization context has nowhere to run: it is
    ///         queued and never executed. Every <c>await</c> here therefore captures the
    ///         context, which costs nothing when there is only one, and returns to the
    ///         Unity main thread - the only thread that may touch a <c>Transform</c> or
    ///         call back across the jslib boundary.
    ///     </para>
    ///     <para>
    ///         The native <c>Zlink.Stream.Connector</c> package does the opposite, and is
    ///         right to: it runs where a thread pool exists. Copying that convention into
    ///         this package stops the connector after its first awaited boundary call, with
    ///         no exception and no log. <c>test/contract/unity-webgl-package.test.js</c>
    ///         fails the build if <c>ConfigureAwait</c> reappears under <c>Runtime/</c>.
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

        private readonly Queue<ZlinkStreamInboundEvent> _inbox =
            new Queue<ZlinkStreamInboundEvent>();
        private readonly Queue<DispatchItem> _dispatchQueue = new Queue<DispatchItem>();
        private readonly Dictionary<int, PendingCall> _pending = new Dictionary<int, PendingCall>();
        private readonly Dictionary<int, string> _observerNames = new Dictionary<int, string>();
        private readonly HashSet<string> _observed = new HashSet<string>(StringComparer.Ordinal);
        private readonly Dictionary<string, List<HandlerRegistration>> _handlers = new Dictionary<
            string,
            List<HandlerRegistration>
        >(StringComparer.Ordinal);
        private readonly ZlinkStreamReceivedMessages _received = new ZlinkStreamReceivedMessages();
        private readonly Dictionary<string, ZlinkStreamActor> _actors = new Dictionary<
            string,
            ZlinkStreamActor
        >(StringComparer.Ordinal);
        private readonly Dictionary<int, ZlinkStreamActor> _actorsByHandle =
            new Dictionary<int, ZlinkStreamActor>();
        private readonly List<ZlinkStreamActor> _actorOrder = new List<ZlinkStreamActor>();
        private readonly List<Action<ZlinkStreamActor>> _actorBoundHandlers =
            new List<Action<ZlinkStreamActor>>();
        private readonly List<Action<ZlinkStreamActor>> _actorUnboundHandlers =
            new List<Action<ZlinkStreamActor>>();
        private readonly List<Action<ZlinkStreamRequestSendingContext>> _requestSendingHandlers =
            new List<Action<ZlinkStreamRequestSendingContext>>();
        private readonly List<Action<ZlinkStreamReplyReceivedContext>> _replyReceivedHandlers =
            new List<Action<ZlinkStreamReplyReceivedContext>>();

        private readonly int _handle;
        private int _nextCallId = 1;
        private Task _advance;
        private ZlinkStreamConnectionState _state = ZlinkStreamConnectionState.Created;
        private ZlinkStreamCloseReason? _closeReason;
        private bool _disposed;

        internal ZlinkStreamWebGlConnector(ZlinkStreamConnectorOptions options)
        {
            if (options is null)
                throw new ArgumentNullException(nameof(options));
            Options = options;
            ValidateOptions(options);

            _handle = ZlinkStreamInterop.Create(BuildOptionsJson(options));
            if (_handle == 0)
                throw CreateFailure();

            Instances[_handle] = this;
            if (
                ZlinkStreamInterop.SetEventSink(
                    _handle,
                    Marshal.GetFunctionPointerForDelegate(SinkDelegate)
                ) == 0
            )
            {
                Instances.Remove(_handle);
                ZlinkStreamInterop.Destroy(_handle);
                throw Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "The WebGL build could not install the ZLink stream event sink."
                );
            }

            Connect = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Connect);
            Close = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Close);
            Dispatch = new ZlinkStreamLifecycleCall(this, ZlinkStreamLifecycleKind.Dispatch);
        }

        public event Func<ZlinkStreamError, CancellationToken, ValueTask> ErrorReceived;

        public event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> Disconnected;

        public event Func<
            ZlinkStreamConnectionStateChanged,
            CancellationToken,
            ValueTask
        > ConnectionStateChanged;

        public bool IsConnected => !_disposed && ZlinkStreamInterop.IsConnected(_handle) != 0;

        public ZlinkStreamConnectionState State =>
            _disposed ? ZlinkStreamConnectionState.Closed : _state;

        public ZlinkStreamConnectorOptions Options { get; }

        public IReadOnlyList<ZlinkStreamActor> Actors => new List<ZlinkStreamActor>(_actorOrder);

        public ZlinkStreamActor Actor(string actorId)
        {
            if (actorId is null)
                throw new ArgumentNullException(nameof(actorId));
            return _actors.TryGetValue(actorId, out var actor) ? actor : null;
        }

        public IDisposable OnActorBound(Action<ZlinkStreamActor> handler)
        {
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            _actorBoundHandlers.Add(handler);
            return new HookRegistration<ZlinkStreamActor>(_actorBoundHandlers, handler);
        }

        public IDisposable OnActorUnbound(Action<ZlinkStreamActor> handler)
        {
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            _actorUnboundHandlers.Add(handler);
            return new HookRegistration<ZlinkStreamActor>(_actorUnboundHandlers, handler);
        }

        public IDisposable OnRequestSending(Action<ZlinkStreamRequestSendingContext> handler)
        {
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            _requestSendingHandlers.Add(handler);
            return new HookRegistration<ZlinkStreamRequestSendingContext>(
                _requestSendingHandlers,
                handler
            );
        }

        // _replyReceivedHandlers is the one owner of whether the JavaScript boundary
        // subscribes to reply-received notifications at all: ZlinkStreamRuntime.jspre
        // used to subscribe unconditionally, which meant every WebGL connector always
        // looked like it had a hook to ZlinkStreamConnector.ts's own no-hook early
        // return, so every request paid to copy the reply context and payload across
        // the boundary whether or not C# had anything registered. Telling JavaScript
        // only on the set's 0/1 transition - not on every Add/Remove - keeps this one
        // decision point instead of adding a second place that tracks interest.
        public IDisposable OnReplyReceived(Action<ZlinkStreamReplyReceivedContext> handler)
        {
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            _replyReceivedHandlers.Add(handler);
            if (_replyReceivedHandlers.Count == 1)
                ZlinkStreamInterop.SetReplyReceivedInterest(_handle, 1);
            return new ReplyReceivedHookRegistration(this, handler);
        }

        /// <summary>Callbacks waiting for the next <see cref="Dispatch" />.</summary>
        public int PendingDispatchCount => _dispatchQueue.Count;

        public IZlinkStreamLifecycleCall Connect { get; }

        public IZlinkStreamLifecycleCall Close { get; }

        public IZlinkStreamLifecycleCall Dispatch { get; }

        internal ZlinkStreamCloseReason? CloseReason => _closeReason;

        public int ReceivedCount(string name)
        {
            EnsureObserved(name);
            return _received.Count(name);
        }

        public IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload)
        {
            ThrowIfDisposed();
            if (payload is null)
                throw new ArgumentNullException(nameof(payload));
            return new ZlinkStreamSendBuilder(this, payload);
        }

        public IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload)
        {
            ThrowIfDisposed();
            if (payload is null)
                throw new ArgumentNullException(nameof(payload));
            return new ZlinkStreamRequestBuilder(this, payload);
        }

        internal IZlinkStreamSendCall SendActor(
            ZlinkStreamActor actor,
            ZlinkStreamEncodedPayload payload
        )
        {
            actor.EnsureBound();
            return new ZlinkStreamSendBuilder(this, payload, actor);
        }

        internal IZlinkStreamRequestCall RequestActor(
            ZlinkStreamActor actor,
            ZlinkStreamEncodedPayload payload
        )
        {
            actor.EnsureBound();
            return new ZlinkStreamRequestBuilder(this, payload, actor);
        }

        internal IDisposable OnActor(
            ZlinkStreamActor actor,
            string name,
            Func<
                ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
                CancellationToken,
                ValueTask
            > handler
        )
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentNullException(nameof(name));
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
            ThrowIfDisposed();
            EnsureObserved(name);
            var registration = new HandlerRegistration(this, name, handler, actor);
            if (!_handlers.TryGetValue(name, out var handlers))
            {
                handlers = new List<HandlerRegistration>();
                _handlers[name] = handlers;
            }

            handlers.Add(registration);
            return registration;
        }

        public IDisposable On(
            string name,
            Func<
                ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
                CancellationToken,
                ValueTask
            > handler
        )
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentNullException(nameof(name));
            if (handler is null)
                throw new ArgumentNullException(nameof(handler));
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
            if (_disposed)
                return;
            try
            {
                await Close.Async();
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

        internal ValueTask RunLifecycleAsync(
            ZlinkStreamLifecycleKind kind,
            CancellationToken cancellationToken
        )
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
            await DriveAsync(callId, pending, false, cancellationToken);
        }

        private async Task RunCloseAsync(CancellationToken cancellationToken)
        {
            var callId = NextCallId();
            var pending = RegisterPending(callId);
            ZlinkStreamInterop.Close(_handle, callId);
            await DriveAsync(callId, pending, false, cancellationToken);
        }

        private async Task RunDispatchAsync(CancellationToken cancellationToken)
        {
            // Reads whatever the transport has, then runs the callbacks that were waiting
            // for it. Only this path runs registered handlers, which is what lets the wait
            // surfaces observe the unread queue without a dispatch pump
            // (stream-connector spec 32 section 7).
            StartAdvanceIfIdle();
            var advance = _advance;
            if (advance != null)
                await advance;
            PumpAndTransfer();
            await RunDispatchQueueAsync(cancellationToken);
        }

        internal async ValueTask SendAsync(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            CancellationToken cancellationToken,
            ZlinkStreamActor actor = null
        )
        {
            var name =
                packetName
                ?? (
                    payload.MessageType is null
                        ? null
                        : Options.NameResolver.Resolve(payload.MessageType)
                );
            if (string.IsNullOrEmpty(name))
                throw Error(ZlinkStreamErrorCode.ValidationFailed, "Send packet name is required.");
            var call = new ZlinkStreamJson.Writer().StartObject();
            call.Number("codec", (int)payload.Codec);
            call.String("packetName", name);
            WriteMetadata(call, metadata);
            call.Bool("compress", compress);
            if (actor != null)
            {
                call.Number("actorHandle", actor.Handle);
                call.String("actorId", actor.ActorId);
            }
            call.EndObject();

            var callId = NextCallId();
            var pending = RegisterPending(callId);
            InvokeWithPayload(
                payload,
                (pointer, length) =>
                    ZlinkStreamInterop.Send(_handle, callId, call.ToString(), pointer, length)
            );
            await DriveAsync(callId, pending, true, cancellationToken);
        }

        internal async ValueTask<ZlinkStreamEncodedPayload> RequestAsync(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            CancellationToken cancellationToken,
            ZlinkStreamActor actor = null
        )
        {
            var callId = StartRequest(
                payload,
                packetName,
                metadata,
                compress,
                timeout,
                out var pending,
                actor
            );
            await DriveAsync(callId, pending, true, cancellationToken);
            return pending.Result;
        }

        internal void SubmitRequest(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback,
            ZlinkStreamActor actor = null
        )
        {
            var callId = StartRequest(
                payload,
                packetName,
                metadata,
                compress,
                timeout,
                out var pending,
                actor
            );
            // Spec 32 section 7: request callbacks run on Dispatch, like push handlers.
            pending.Callback = callback;
            if (pending.Error != null)
                DispatchOrQueue(
                    DispatchItem.ForRequestResult(
                        callback,
                        ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure(pending.Error)
                    )
                );
            _ = DriveQuietlyAsync(callId, pending);
        }

        private int StartRequest(
            ZlinkStreamEncodedPayload payload,
            string packetName,
            ZlinkStreamMetadata metadata,
            bool compress,
            TimeSpan? timeout,
            out PendingCall pending,
            ZlinkStreamActor actor = null
        )
        {
            ThrowIfDisposed();
            var name =
                packetName
                ?? (
                    payload.MessageType is null
                        ? null
                        : Options.NameResolver.Resolve(payload.MessageType)
                );
            if (string.IsNullOrEmpty(name))
                throw Error(
                    ZlinkStreamErrorCode.ValidationFailed,
                    "Request packet name is required."
                );
            var elapsed = Stopwatch.StartNew();
            var context = new ZlinkStreamRequestSendingContext(name, actor?.ActorId, metadata);
            foreach (var handler in _requestSendingHandlers.ToArray())
            {
                try
                {
                    handler(context);
                }
                catch (Exception exception)
                {
                    ReportHookFailure(exception);
                }
            }
            var call = new ZlinkStreamJson.Writer().StartObject();
            call.Number("codec", (int)payload.Codec);
            call.String("packetName", name);
            WriteMetadata(call, context.Metadata);
            call.Bool("compress", compress);
            call.Number("timeoutMs", (timeout ?? Options.RequestTimeout).TotalMilliseconds);
            if (actor != null)
            {
                call.Number("actorHandle", actor.Handle);
                call.String("actorId", actor.ActorId);
            }
            call.EndObject();
            var callId = NextCallId();
            pending = RegisterPending(callId);
            try
            {
                var json = call.ToString();
                InvokeWithPayload(
                    payload,
                    (pointer, length) =>
                        ZlinkStreamInterop.Request(_handle, callId, json, pointer, length)
                );
            }
            catch (Exception exception)
            {
                var error = exception is ZlinkStreamException streamException
                    ? streamException.Error
                    : new ZlinkStreamError(
                        ZlinkStreamErrorCode.SendFailed,
                        exception.Message,
                        exception
                    );
                pending.Fail(error);
                elapsed.Stop();
                RouteLocalReplyFailure(name, actor?.ActorId, error, elapsed.Elapsed);
            }
            return callId;
        }

        private async Task DriveQuietlyAsync(int callId, PendingCall pending)
        {
            try
            {
                await DriveAsync(callId, pending, true, CancellationToken.None);
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
            CancellationToken cancellationToken
        )
        {
            try
            {
                while (!pending.Completed)
                {
                    if (advanceTransport && IsConnected)
                        StartAdvanceIfIdle();
                    PumpAndTransfer();
                    if (pending.Completed)
                        break;
                    if (cancellationToken.IsCancellationRequested)
                    {
                        ZlinkStreamInterop.Cancel(_handle, callId);
                        cancellationToken.ThrowIfCancellationRequested();
                    }

                    await Task.Yield();
                }

                if (pending.Error != null)
                    throw new ZlinkStreamException(pending.Error);
            }
            finally
            {
                _pending.Remove(callId);
            }
        }

        private void StartAdvanceIfIdle()
        {
            if (_advance != null && !_advance.IsCompleted)
                return;
            var callId = NextCallId();
            var pending = RegisterPending(callId);
            ZlinkStreamInterop.Dispatch(_handle, callId);
            _advance = DriveAsync(callId, pending, false, CancellationToken.None);
            _advance.ContinueWith(
                static task =>
                {
                    _ = task.Exception;
                },
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously
            );
        }

        /// <summary>
        ///     Moves queued JavaScript events into managed state. Safe to call anywhere: it
        ///     runs no user code.
        /// </summary>
        private void PumpAndTransfer()
        {
            if (_disposed)
                return;
            var drained = ZlinkStreamInterop.Pump(_handle, MaxEventsPerPump);
            // A refusal means an outer pump on this stack is draining the same queue,
            // so there is nothing to do. A failure means the boundary stopped
            // delivering, and staying quiet about it would leave every caller waiting
            // for events that are no longer coming.
            if (drained == ZlinkStreamInterop.PumpFailed)
                throw BoundaryFailure();
            while (_inbox.Count > 0)
                Transfer(_inbox.Dequeue());
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
                    : "The ZLink WebGL stream boundary failed while draining events: " + text
            );
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
                    DispatchOrQueue(DispatchItem.ForError(ParseError(inbound.Text)));
                    break;
                case ZlinkStreamInterop.EventDisconnected:
                    _closeReason = ParseCloseReason(inbound.Text);
                    DispatchOrQueue(
                        DispatchItem.ForDisconnected(
                            new ZlinkStreamDisconnected(
                                _closeReason ?? ZlinkStreamCloseReason.TransportError
                            )
                        )
                    );
                    break;
                case ZlinkStreamInterop.EventStateChanged:
                    var change = ParseStateChange(inbound.Text);
                    _state = change.Current;
                    DispatchOrQueue(DispatchItem.ForStateChange(change));
                    break;
                case ZlinkStreamInterop.EventActorBound:
                    RouteActorBound(inbound.Text);
                    break;
                case ZlinkStreamInterop.EventActorUnbound:
                    RouteActorUnbound(inbound.Text);
                    break;
                case ZlinkStreamInterop.EventReplyReceived:
                    RouteReplyReceived(inbound);
                    break;
            }
        }

        private void CompleteCall(ZlinkStreamInboundEvent inbound)
        {
            if (!_pending.TryGetValue(inbound.Id, out var pending))
                return;
            if (inbound.Value == 1)
            {
                ZlinkStreamEncodedPayload payload = null;
                if (inbound.Bytes != null)
                {
                    var codec = (ZlinkStreamCodec)
                        ZlinkStreamJson.Parse(inbound.Text).IntOf("codec", 0);
                    payload = new ZlinkStreamEncodedPayload(codec, inbound.Bytes);
                }

                pending.Complete(payload);
                if (pending.Callback != null)
                {
                    DispatchOrQueue(
                        DispatchItem.ForRequestResult(
                            pending.Callback,
                            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
                                payload
                                    ?? new ZlinkStreamEncodedPayload(
                                        ZlinkStreamCodec.Raw,
                                        ReadOnlyMemory<byte>.Empty
                                    )
                            )
                        )
                    );
                }

                return;
            }

            var error = ParseError(inbound.Text);
            pending.Fail(error);
            if (pending.Callback != null)
            {
                DispatchOrQueue(
                    DispatchItem.ForRequestResult(
                        pending.Callback,
                        ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure(error)
                    )
                );
            }
        }

        private void RouteReplyReceived(ZlinkStreamInboundEvent inbound)
        {
            var node = ZlinkStreamJson.Parse(inbound.Text);
            var succeeded = node.Member("succeeded")?.Flag == true;
            var reply = succeeded
                ? new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
                    node.TextOf("name") ?? string.Empty,
                    ReadMetadata(node.Member("metadata")),
                    new ZlinkStreamEncodedPayload(
                        (ZlinkStreamCodec)inbound.Value,
                        inbound.Bytes ?? ReadOnlyMemory<byte>.Empty
                    ),
                    node.TextOf("actorId")
                )
                : null;
            var errorNode = node.Member("error");
            var error =
                succeeded || errorNode is null
                    ? null
                    : new ZlinkStreamError(
                        ParseErrorCode(errorNode.TextOf("code")),
                        errorNode.TextOf("message") ?? string.Empty
                    );
            var context = new ZlinkStreamReplyReceivedContext(
                node.TextOf("requestPacketName"),
                node.TextOf("actorId"),
                reply,
                error,
                TimeSpan.FromMilliseconds(node.Member("elapsed")?.Number ?? 0)
            );
            foreach (var handler in _replyReceivedHandlers.ToArray())
            {
                DispatchOrQueue(
                    DispatchItem.ForCallback(() =>
                    {
                        try
                        {
                            handler(context);
                        }
                        catch (Exception exception)
                        {
                            ReportHookFailure(exception);
                        }
                    })
                );
            }
        }

        private void RouteLocalReplyFailure(
            string name,
            string actorId,
            ZlinkStreamError error,
            TimeSpan elapsed
        )
        {
            var context = new ZlinkStreamReplyReceivedContext(name, actorId, null, error, elapsed);
            foreach (var handler in _replyReceivedHandlers.ToArray())
                DispatchOrQueue(
                    DispatchItem.ForCallback(() =>
                    {
                        try
                        {
                            handler(context);
                        }
                        catch (Exception exception)
                        {
                            ReportHookFailure(exception);
                        }
                    })
                );
        }

        private void ReportHookFailure(Exception exception)
        {
            DispatchOrQueue(
                DispatchItem.ForError(
                    new ZlinkStreamError(
                        ZlinkStreamErrorCode.UserCallbackFailed,
                        "Request hook failed.",
                        exception
                    )
                )
            );
        }

        private void RouteMessage(ZlinkStreamInboundEvent inbound)
        {
            if (!_observerNames.TryGetValue(inbound.Id, out var observedName))
                return;
            var node = ZlinkStreamJson.Parse(inbound.Text);
            var name = node.TextOf("name") ?? observedName;
            var metadata = ReadMetadata(node.Member("metadata"));
            var payload = new ZlinkStreamEncodedPayload(
                (ZlinkStreamCodec)inbound.Value,
                inbound.Bytes ?? ReadOnlyMemory<byte>.Empty
            );
            var message = new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
                name,
                metadata,
                payload,
                node.TextOf("actorId")
            );
            var actorHandle = node.IntOf("actorHandle", 0);
            _actorsByHandle.TryGetValue(actorHandle, out var messageActor);

            // Same rule as the native connector: a registered handler takes the message,
            // otherwise it waits in the unread history for a wait surface.
            if (_handlers.TryGetValue(name, out var handlers) && handlers.Count > 0)
            {
                DispatchOrQueue(DispatchItem.ForMessage(message, messageActor));
                return;
            }

            _received.Record(message);
        }

        internal async ValueTask InvokeMessageHandlers(
            ZlinkStreamMessage<ZlinkStreamEncodedPayload> message,
            ZlinkStreamActor messageActor,
            CancellationToken cancellationToken
        )
        {
            if (!_handlers.TryGetValue(message.Name, out var handlers))
            {
                _received.Record(message);
                return;
            }

            var matching = new List<HandlerRegistration>(handlers.Count);
            foreach (var handler in handlers)
            {
                if (handler.Actor is null || handler.Actor == messageActor)
                    matching.Add(handler);
            }
            if (matching.Count == 0)
            {
                _received.Record(message);
                return;
            }

            foreach (var handler in matching)
                await handler.Handler(message, cancellationToken);
        }

        private void RouteActorBound(string json)
        {
            var node = ZlinkStreamJson.Parse(json);
            var actorId = node.TextOf("actorId");
            var actorHandle = node.IntOf("actorHandle", 0);
            if (
                string.IsNullOrEmpty(actorId)
                || actorHandle <= 0
                || _actors.ContainsKey(actorId)
                || _actorsByHandle.ContainsKey(actorHandle)
            )
                return;
            var actor = new ZlinkStreamActor(this, actorId, actorHandle);
            _actors.Add(actorId, actor);
            _actorsByHandle.Add(actorHandle, actor);
            _actorOrder.Add(actor);
            DispatchOrQueue(
                DispatchItem.ForCallback(() => InvokeActorHandlers(_actorBoundHandlers, actor))
            );
        }

        private void RouteActorUnbound(string json)
        {
            var node = ZlinkStreamJson.Parse(json);
            var actorHandle = node.IntOf("actorHandle", 0);
            if (actorHandle <= 0 || !_actorsByHandle.TryGetValue(actorHandle, out var actor))
                return;
            _actorsByHandle.Remove(actorHandle);
            if (_actors.TryGetValue(actor.ActorId, out var current) && current == actor)
                _actors.Remove(actor.ActorId);
            _actorOrder.Remove(actor);
            actor.Close();
            DispatchOrQueue(
                DispatchItem.ForCallback(() => InvokeActorHandlers(_actorUnboundHandlers, actor))
            );
        }

        private void DispatchOrQueue(DispatchItem item)
        {
            if (Options.DispatchMode == ZlinkStreamDispatchMode.Manual)
            {
                _dispatchQueue.Enqueue(item);
                return;
            }

            _ = InvokeDispatchItemAsync(item, CancellationToken.None);
        }

        private static void InvokeActorHandlers(
            List<Action<ZlinkStreamActor>> handlers,
            ZlinkStreamActor actor
        )
        {
            foreach (var handler in handlers.ToArray())
                handler(actor);
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
                await InvokeDispatchItemAsync(item, cancellationToken);
            }
        }

        private async Task InvokeDispatchItemAsync(
            DispatchItem item,
            CancellationToken cancellationToken
        )
        {
            try
            {
                await item.InvokeAsync(this, cancellationToken);
            }
            catch (Exception exception)
            {
                var handler = ErrorReceived;
                if (handler is null)
                    return;
                var error = new ZlinkStreamError(
                    ZlinkStreamErrorCode.UserCallbackFailed,
                    "Stream callback failed.",
                    exception
                );
                try
                {
                    await handler(error, cancellationToken);
                }
                catch (Exception)
                {
                    // A failing error handler must not abort the dispatch pass.
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
            CancellationToken cancellationToken
        )
        {
            ThrowIfDisposed();
            EnsureObserved(name);
            var elapsed = Stopwatch.StartNew();
            while (true)
            {
                if (IsConnected)
                    StartAdvanceIfIdle();
                PumpAndTransfer();
                var message = _received.TryTake(name, predicate);
                if (message != null)
                    return message;
                cancellationToken.ThrowIfCancellationRequested();
                if (elapsed.Elapsed >= timeout)
                    throw new TimeoutException($"Timed out waiting for '{name}' stream message.");
                await Task.Yield();
            }
        }

        internal void EnsureObserved(string name)
        {
            if (string.IsNullOrEmpty(name))
                throw new ArgumentNullException(nameof(name));
            if (_disposed || _observed.Contains(name))
                return;
            var observerId = ZlinkStreamInterop.Observe(_handle, name);
            if (observerId == 0)
                return;
            _observed.Add(name);
            _observerNames[observerId] = name;
        }

        internal void RemoveHandler(string name, HandlerRegistration registration)
        {
            if (!_handlers.TryGetValue(name, out var handlers))
                return;
            handlers.Remove(registration);
            if (handlers.Count == 0)
                _handlers.Remove(name);
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
            int bytesLength
        )
        {
            // Runs on the JavaScript stack, inside ZlinkStreamPump. It copies and returns:
            // no user code, no await, no exception. The text and bytes pointers are freed
            // by the caller as soon as this returns, so both are copied here.
            try
            {
                if (!Instances.TryGetValue(handle, out var connector))
                    return;
                var copiedText = text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text);
                byte[] copiedBytes = null;
                if (bytes != IntPtr.Zero && bytesLength > 0)
                {
                    copiedBytes = new byte[bytesLength];
                    Marshal.Copy(bytes, copiedBytes, 0, bytesLength);
                }

                connector._inbox.Enqueue(
                    new ZlinkStreamInboundEvent(eventType, id, value, copiedText, copiedBytes)
                );
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
            if (_disposed)
                throw new ObjectDisposedException(nameof(IZlinkStreamConnector));
        }

        private static void InvokeWithPayload(
            ZlinkStreamEncodedPayload payload,
            Action<IntPtr, int> call
        )
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

        private static void WriteMetadata(
            ZlinkStreamJson.Writer writer,
            ZlinkStreamMetadata metadata
        )
        {
            if (metadata is null || metadata.Count == 0)
                return;
            writer.Name("metadata").StartObject();
            foreach (var pair in metadata.Values)
                writer.String(pair.Key, pair.Value);
            writer.EndObject();
        }

        private static ZlinkStreamMetadata ReadMetadata(ZlinkStreamJson.Node node)
        {
            if (node is null)
                return ZlinkStreamMetadata.Empty;
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var pair in node.Members())
            {
                if (pair.Value?.Text != null)
                    values[pair.Key] = pair.Value.Text;
            }

            return ZlinkStreamMetadata.FromDictionary(values);
        }

        private static ZlinkStreamError ParseError(string json)
        {
            var node = ZlinkStreamJson.Parse(json);
            return new ZlinkStreamError(
                ParseErrorCode(node.TextOf("code")),
                node.TextOf("message") ?? "Stream operation failed."
            );
        }

        private static ZlinkStreamErrorCode ParseErrorCode(string code)
        {
            switch (code)
            {
                case "disconnected":
                    return ZlinkStreamErrorCode.Disconnected;
                case "configurationError":
                    return ZlinkStreamErrorCode.ConfigurationError;
                case "validationFailed":
                    return ZlinkStreamErrorCode.ValidationFailed;
                case "requestTimeout":
                    return ZlinkStreamErrorCode.RequestTimeout;
                case "connectTimeout":
                    return ZlinkStreamErrorCode.ConnectTimeout;
                case "frameDecodeFailed":
                    return ZlinkStreamErrorCode.FrameDecodeFailed;
                case "frameTooLarge":
                    return ZlinkStreamErrorCode.FrameTooLarge;
                case "compressionFailed":
                    return ZlinkStreamErrorCode.CompressionFailed;
                case "decompressionFailed":
                    return ZlinkStreamErrorCode.DecompressionFailed;
                case "tlsValidationFailed":
                    return ZlinkStreamErrorCode.TlsValidationFailed;
                case "userCallbackFailed":
                    return ZlinkStreamErrorCode.UserCallbackFailed;
                case "remoteError":
                    return ZlinkStreamErrorCode.RemoteError;
                default:
                    return ZlinkStreamErrorCode.SendFailed;
            }
        }

        private static ZlinkStreamCloseReason? ParseCloseReason(string json)
        {
            switch (ZlinkStreamJson.Parse(json).TextOf("closeReason"))
            {
                case "ClientClose":
                    return ZlinkStreamCloseReason.ClientClose;
                case "IdleTimeout":
                    return ZlinkStreamCloseReason.IdleTimeout;
                case "HeartbeatTimeout":
                    return ZlinkStreamCloseReason.HeartbeatTimeout;
                case "ServerDrain":
                    return ZlinkStreamCloseReason.ServerDrain;
                case "ProtocolError":
                    return ZlinkStreamCloseReason.ProtocolError;
                case "TransportError":
                    return ZlinkStreamCloseReason.TransportError;
                default:
                    return null;
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
                    errorNode.TextOf("message") ?? string.Empty
                );
            }

            return new ZlinkStreamConnectionStateChanged(
                ParseState(node.TextOf("previous")),
                ParseState(node.TextOf("current")),
                error
            );
        }

        private static ZlinkStreamConnectionState ParseState(string state)
        {
            switch (state)
            {
                case "connecting":
                    return ZlinkStreamConnectionState.Connecting;
                case "connected":
                    return ZlinkStreamConnectionState.Connected;
                case "reconnecting":
                    return ZlinkStreamConnectionState.Reconnecting;
                case "disconnected":
                    return ZlinkStreamConnectionState.Disconnected;
                case "closed":
                    return ZlinkStreamConnectionState.Closed;
                default:
                    return ZlinkStreamConnectionState.Created;
            }
        }

        private static void ValidateOptions(ZlinkStreamConnectorOptions options)
        {
            if (options.Endpoint is null)
                throw Error(ZlinkStreamErrorCode.ConfigurationError, "Endpoint is required.");
            if (options.CompressionCodec != null)
                throw Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "CompressionCodec is not supported on WebGL: the compression codec runs in the "
                        + "JavaScript connector. Select the algorithm with Compression instead."
                );
            if (options.SkipServerCertificateValidation)
                throw Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "SkipServerCertificateValidation is not supported on WebGL: the browser owns "
                        + "certificate validation for wss:// and offers no way to skip it."
                );
        }

        private static string BuildOptionsJson(ZlinkStreamConnectorOptions options)
        {
            var writer = new ZlinkStreamJson.Writer().StartObject();
            writer.String("endpoint", options.Endpoint.ToString());
            if (options.Transport.HasValue)
            {
                writer.String(
                    "transport",
                    options.Transport.Value == ZlinkStreamTransport.WebSocketSecure
                        ? "webSocketSecure"
                        : "webSocket"
                );
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
            writer.String(
                "dispatchMode",
                options.DispatchMode == ZlinkStreamDispatchMode.Immediate ? "immediate" : "manual"
            );
            writer.String(
                "compression",
                options.Compression == ZlinkStreamCompression.None ? "none" : "lz4"
            );
            writer.EndObject();
            return writer.ToString();
        }

        private static ZlinkStreamException CreateFailure()
        {
            var text = ZlinkStreamInterop.TakeLastErrorText();
            if (string.IsNullOrEmpty(text))
            {
                return Error(
                    ZlinkStreamErrorCode.ConfigurationError,
                    "The stream connector could not be created."
                );
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
                Func<
                    ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
                    CancellationToken,
                    ValueTask
                > handler,
                ZlinkStreamActor actor = null
            )
            {
                _connector = connector;
                _name = name;
                Handler = handler;
                Actor = actor;
            }

            internal Func<
                ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
                CancellationToken,
                ValueTask
            > Handler { get; }

            internal ZlinkStreamActor Actor { get; }

            public void Dispose()
            {
                _connector.RemoveHandler(_name, this);
            }
        }

        private sealed class HookRegistration<TContext> : IDisposable
        {
            private readonly List<Action<TContext>> _handlers;
            private readonly Action<TContext> _handler;

            internal HookRegistration(List<Action<TContext>> handlers, Action<TContext> handler)
            {
                _handlers = handlers;
                _handler = handler;
            }

            public void Dispose()
            {
                _handlers.Remove(_handler);
            }
        }

        // Not a HookRegistration<T>: disposing the last reply received hook has to
        // tell JavaScript interest dropped to zero (see OnReplyReceived), which none
        // of the other hook kinds need.
        private sealed class ReplyReceivedHookRegistration : IDisposable
        {
            private readonly ZlinkStreamWebGlConnector _connector;
            private readonly Action<ZlinkStreamReplyReceivedContext> _handler;

            internal ReplyReceivedHookRegistration(
                ZlinkStreamWebGlConnector connector,
                Action<ZlinkStreamReplyReceivedContext> handler
            )
            {
                _connector = connector;
                _handler = handler;
            }

            public void Dispose()
            {
                // Remove's own result, not a re-check of Count: a second Dispose of
                // the same registration removes nothing, so it must not repeat the
                // interop call - Count alone cannot tell "just transitioned to zero"
                // from "already zero and unrelated hooks came and went since".
                if (!_connector._replyReceivedHandlers.Remove(_handler))
                    return;
                if (_connector._replyReceivedHandlers.Count == 0)
                    ZlinkStreamInterop.SetReplyReceivedInterest(_connector._handle, 0);
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
            public ZlinkStreamInboundEvent(
                int eventType,
                int id,
                int value,
                string text,
                byte[] bytes
            )
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
            private readonly ZlinkStreamActor _messageActor;
            private readonly ZlinkStreamMessage<ZlinkStreamEncodedPayload> _message;
            private readonly ZlinkStreamError _error;
            private readonly ZlinkStreamDisconnected _disconnected;
            private readonly ZlinkStreamConnectionStateChanged _stateChange;
            private readonly Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> _callback;
            private readonly ZlinkStreamResult<ZlinkStreamEncodedPayload> _result;
            private readonly Action _action;

            private DispatchItem(
                int kind,
                ZlinkStreamActor messageActor,
                ZlinkStreamMessage<ZlinkStreamEncodedPayload> message,
                ZlinkStreamError error,
                ZlinkStreamDisconnected disconnected,
                ZlinkStreamConnectionStateChanged stateChange,
                Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback,
                ZlinkStreamResult<ZlinkStreamEncodedPayload> result,
                Action action = null
            )
            {
                _kind = kind;
                _messageActor = messageActor;
                _message = message;
                _error = error;
                _disconnected = disconnected;
                _stateChange = stateChange;
                _callback = callback;
                _result = result;
                _action = action;
            }

            public static DispatchItem ForMessage(
                ZlinkStreamMessage<ZlinkStreamEncodedPayload> message,
                ZlinkStreamActor actor
            )
            {
                return new DispatchItem(1, actor, message, null, null, null, null, default);
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
                ZlinkStreamResult<ZlinkStreamEncodedPayload> result
            )
            {
                return new DispatchItem(5, null, null, null, null, null, callback, result);
            }

            public static DispatchItem ForCallback(Action callback)
            {
                return new DispatchItem(6, null, null, null, null, null, null, default, callback);
            }

            public async ValueTask InvokeAsync(
                ZlinkStreamWebGlConnector connector,
                CancellationToken cancellationToken
            )
            {
                switch (_kind)
                {
                    case 1:
                        await connector.InvokeMessageHandlers(
                            _message,
                            _messageActor,
                            cancellationToken
                        );
                        break;
                    case 2:
                    {
                        var handler = connector.ErrorReceived;
                        if (handler != null)
                            await handler(_error, cancellationToken);
                        break;
                    }

                    case 3:
                    {
                        var handler = connector.Disconnected;
                        if (handler != null)
                            await handler(_disconnected, cancellationToken);
                        break;
                    }

                    case 4:
                    {
                        var handler = connector.ConnectionStateChanged;
                        if (handler != null)
                            await handler(_stateChange, cancellationToken);
                        break;
                    }

                    case 5:
                        _callback(_result);
                        break;
                    default:
                        _action();
                        break;
                }
            }
        }
    }

    internal enum ZlinkStreamLifecycleKind
    {
        Connect,
        Close,
        Dispatch,
    }
}
