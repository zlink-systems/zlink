package systems.zlink.framework.kotlin

import java.lang.ref.WeakReference
import java.time.Duration
import java.util.WeakHashMap
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import kotlin.coroutines.CoroutineContext
import kotlin.reflect.KClass
import kotlinx.coroutines.ThreadContextElement
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.buffer
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.future.await
import systems.zlink.framework.kotlin.stream.ZLinkKotlinRequestCall as ZLinkKotlinStreamRequestCall
import systems.zlink.stream.connector.ZLinkStreamActor
import systems.zlink.stream.connector.ZLinkStreamAssert
import systems.zlink.stream.connector.ZLinkStreamCloseReason
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamCompressionCodecs
import systems.zlink.stream.connector.ZLinkStreamConnectionState
import systems.zlink.stream.connector.ZLinkStreamConnectionStateHandler
import systems.zlink.stream.connector.ZLinkStreamConnector
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDiagnosticsLevel
import systems.zlink.stream.connector.ZLinkStreamDisconnectedHandler
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload
import systems.zlink.stream.connector.ZLinkStreamError
import systems.zlink.stream.connector.ZLinkStreamErrorHandler
import systems.zlink.stream.connector.ZLinkStreamExpectNoneCall
import systems.zlink.stream.connector.ZLinkStreamFlow
import systems.zlink.stream.connector.ZLinkStreamFlowScope
import systems.zlink.stream.connector.ZLinkStreamLifecycleCall
import systems.zlink.stream.connector.ZLinkStreamMessage
import systems.zlink.stream.connector.ZLinkStreamMessageHandler
import systems.zlink.stream.connector.ZLinkStreamRequestCall
import systems.zlink.stream.connector.ZLinkStreamSendCall
import systems.zlink.stream.connector.ZLinkStreamSequenceCall
import systems.zlink.stream.connector.ZLinkStreamWaitCall
import systems.zlink.stream.connector.ZLinkTypedStreamRequestCall
import systems.zlink.stream.connector.ZLinkTypedStreamSendCall

fun ZLinkStreamConnector.kotlin(): ZLinkKotlinStreamConnector = ZLinkKotlinStreamConnector(this)

fun ZLinkStreamConnectorOptions.withDefaultStreamCompression(): ZLinkStreamConnectorOptions =
    withStreamCompression(ZLinkStreamCompressionCodecs.lz4())

fun ZLinkStreamConnectorOptions.withLz4StreamCompression(): ZLinkStreamConnectorOptions =
    withDefaultStreamCompression()

fun ZLinkStreamConnectorOptions.withStreamCompression(
    codec: ZLinkStreamCompressionCodec
): ZLinkStreamConnectorOptions = copyStreamCompression(ZLinkStreamCompression.LZ4, codec)

fun ZLinkStreamConnectorOptions.withoutStreamCompression(): ZLinkStreamConnectorOptions =
    copyStreamCompression(ZLinkStreamCompression.NONE, null)

private fun ZLinkStreamConnectorOptions.copyStreamCompression(
    compression: ZLinkStreamCompression,
    codec: ZLinkStreamCompressionCodec?,
): ZLinkStreamConnectorOptions =
    ZLinkStreamConnectorOptions(
        endpoint(),
        dispatchMode(),
        requestTimeout(),
        waitTimeout(),
        maxReconnectAttempts(),
        connectTimeout(),
        maxSendPayloadSize(),
        maxReceivePayloadSize(),
        heartbeatEnabled(),
        heartbeatInterval(),
        heartbeatTimeout(),
        reconnectEnabled(),
        reconnectInitialDelay(),
        reconnectMaxDelay(),
        reconnectBackoffFactor(),
        skipServerCertificateValidation(),
        compression,
        codec,
        nameResolver(),
        typedCodec(),
        diagnosticsLevel(),
    )

class ZLinkKotlinStreamConnector(@PublishedApi internal val inner: ZLinkStreamConnector) {
    private val actorHandles =
        WeakHashMap<ZLinkStreamActor, WeakReference<ZLinkKotlinStreamActor>>()

    private fun wrapActor(actor: ZLinkStreamActor): ZLinkKotlinStreamActor =
        synchronized(actorHandles) {
            actorHandles[actor]?.get()
                ?: ZLinkKotlinStreamActor(actor).also { actorHandles[actor] = WeakReference(it) }
        }

    val isConnected: Boolean
        get() = inner.isConnected

    val state: ZLinkStreamConnectionState
        get() = inner.state()

    val options: ZLinkStreamConnectorOptions
        get() = inner.options()

    /**
     * The reason the connection last ended, or `null` when it has never ended (common connector
     * spec 32 §6.2, Java/Kotlin spec §12). The Java connector returns an [java.util.Optional];
     * Kotlin reads the same value as a nullable, so code that holds only this wrapper reaches the
     * reason without pulling [inner] back out.
     */
    fun closeReason(): ZLinkStreamCloseReason? = inner.closeReason().orElse(null)

    //  Runtime-mutable diagnostics level (server spec 26 §4.1 / common
    //  connector spec §13). `inner.diagnosticsLevel()` / `setDiagnosticsLevel`
    //  do not follow the `getX`/`setX` naming Kotlin needs to synthesize a
    //  property automatically, so this wrapper exposes the pair explicitly.
    var diagnosticsLevel: ZLinkStreamDiagnosticsLevel
        get() = inner.diagnosticsLevel()
        set(value) = inner.setDiagnosticsLevel(value)

    /**
     * The suspending pair of the [diagnosticsLevel] setter (common connector spec 32 13). It
     * changes the same value; the property setter stays the synchronous surface and does not wait
     * for this one.
     */
    suspend fun setDiagnosticsLevel(level: ZLinkStreamDiagnosticsLevel) {
        inner.setDiagnosticsLevelAsync(level).await()
    }

    val pendingDispatchCount: Int
        get() = inner.pendingDispatchCount()

    fun receivedCount(name: String): Int = inner.receivedCount(name)

    fun on(
        name: String,
        handler: ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>,
    ): AutoCloseable = inner.on(name, handler)

    inline fun <reified TPayload> on(handler: ZLinkStreamMessageHandler<TPayload>): AutoCloseable =
        inner.on(TPayload::class.java, handler)

    fun onErrorReceived(handler: ZLinkStreamErrorHandler): AutoCloseable =
        inner.onErrorReceived(handler)

    fun onDisconnected(handler: ZLinkStreamDisconnectedHandler): AutoCloseable =
        inner.onDisconnected(handler)

    fun onConnectionStateChanged(handler: ZLinkStreamConnectionStateHandler): AutoCloseable =
        inner.onConnectionStateChanged(handler)

    fun connect(): ZLinkKotlinLifecycleCall = ZLinkKotlinLifecycleCall(inner.connect())

    fun close(): ZLinkKotlinLifecycleCall = ZLinkKotlinLifecycleCall(inner.close())

    fun dispatch(): ZLinkKotlinLifecycleCall = ZLinkKotlinLifecycleCall(inner.dispatch())

    fun send(payload: ZLinkStreamEncodedPayload): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(inner.send(payload))

    fun send(payload: Any): ZLinkKotlinSendCall = ZLinkKotlinSendCall(inner.send(payload))

    fun request(payload: ZLinkStreamEncodedPayload): ZLinkKotlinRawRequestCall =
        ZLinkKotlinRawRequestCall(inner.request(payload))

    fun <TReply : Any> request(
        payload: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinStreamRequestCall<TReply> =
        ZLinkKotlinStreamRequestCall(inner.request(payload), replyType)

    inline fun <reified TPayload> waitFor(): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.waitFor(TPayload::class.java), TPayload::class.java)

    inline fun <reified TPayload> waitFor(name: String): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.waitFor(name), TPayload::class.java)

    /**
     * Expects no message of [TPayload] with the name its own resolution rules give it (Java/Kotlin
     * spec §12, §5). The named overload stays for a packet whose name does not follow from the
     * type.
     */
    inline fun <reified TPayload> expectNone(): ZLinkStreamTypedExpectNoneCall<TPayload> =
        ZLinkStreamTypedExpectNoneCall(inner.expectNone(TPayload::class.java))

    inline fun <reified TPayload> expectNone(
        name: String
    ): ZLinkStreamTypedExpectNoneCall<TPayload> =
        ZLinkStreamTypedExpectNoneCall(inner.expectNone(name))

    /**
     * Waits for a sequence of [TPayload] with the name its own resolution rules give it
     * (Java/Kotlin spec §12, §5). The named overload stays for a packet whose name does not follow
     * from the type.
     */
    inline fun <reified TPayload> waitForSequence(): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(
            inner.waitForSequence(TPayload::class.java),
            TPayload::class.java,
        )

    inline fun <reified TPayload> waitForSequence(
        name: String
    ): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.waitForSequence(name), TPayload::class.java)

    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> =
        inner.messages(packetName)

    fun errors(): Flow<ZLinkStreamError> = inner.errors()

    fun actors(): List<ZLinkKotlinStreamActor> = inner.actors().map(::wrapActor)

    fun actor(actorId: String): ZLinkKotlinStreamActor? =
        inner.actor(actorId).orElse(null)?.let(::wrapActor)

    fun actorBound(): Flow<ZLinkKotlinStreamActor> =
        callbackFlow {
                val registration =
                    inner.onActorBound { actor ->
                        trySend(wrapActor(actor))
                        CompletableFuture.completedFuture(null)
                    }
                awaitClose { registration.close() }
            }
            .buffer(Channel.UNLIMITED)

    fun actorUnbound(): Flow<ZLinkKotlinStreamActor> =
        callbackFlow {
                val registration =
                    inner.onActorUnbound { actor ->
                        trySend(wrapActor(actor))
                        CompletableFuture.completedFuture(null)
                    }
                awaitClose { registration.close() }
            }
            .buffer(Channel.UNLIMITED)
}

class ZLinkKotlinStreamActor internal constructor(private val inner: ZLinkStreamActor) {
    val actorId: String
        get() = inner.actorId()

    val isBound: Boolean
        get() = inner.isBound

    fun send(payload: ZLinkStreamEncodedPayload): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(inner.send(payload))

    fun send(payload: Any): ZLinkKotlinSendCall = ZLinkKotlinSendCall(inner.send(payload))

    fun request(payload: ZLinkStreamEncodedPayload): ZLinkKotlinRawRequestCall =
        ZLinkKotlinRawRequestCall(inner.request(payload))

    fun <TReply : Any> request(
        payload: Any,
        replyType: KClass<TReply>,
    ): ZLinkKotlinStreamRequestCall<TReply> =
        ZLinkKotlinStreamRequestCall(inner.request(payload), replyType)

    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> =
        callbackFlow {
            val registration =
                inner.on(packetName) { message ->
                    if (trySend(message).isFailure) {
                        message.payload().payload().close()
                    }
                    CompletableFuture.completedFuture(null)
                }
            awaitClose { registration.close() }
        }
}

inline fun <reified TReply : Any> ZLinkKotlinStreamActor.request(
    payload: Any
): ZLinkKotlinStreamRequestCall<TReply> = request(payload, TReply::class)

inline fun <reified TReply : Any> ZLinkKotlinStreamConnector.request(
    payload: Any
): ZLinkKotlinStreamRequestCall<TReply> = request(payload, TReply::class)

class ZLinkKotlinRawRequestCall(private val inner: ZLinkStreamRequestCall) {
    fun packetName(name: String): ZLinkKotlinRawRequestCall =
        ZLinkKotlinRawRequestCall(inner.packetName(name))

    fun metadata(key: String, value: String): ZLinkKotlinRawRequestCall =
        ZLinkKotlinRawRequestCall(inner.metadata(key, value))

    fun timeout(timeout: Duration): ZLinkKotlinRawRequestCall =
        ZLinkKotlinRawRequestCall(inner.timeout(timeout))

    fun compress(): ZLinkKotlinRawRequestCall = ZLinkKotlinRawRequestCall(inner.compress())

    suspend fun await(): ZLinkStreamEncodedPayload = inner.submit().await()
}

class ZLinkKotlinLifecycleCall(private val inner: ZLinkStreamLifecycleCall) {
    suspend fun await() {
        inner.submit().await()
    }
}

/**
 * The Kotlin one-way send builder (Java spec 03 12). It carries the same
 * `packetName`/`metadata`/`compress` steps the Java `ZLinkStreamSendCall` has; without them a
 * Kotlin caller that needs any of those has to drop out of the wrapper and use the Java call.
 */
class ZLinkKotlinSendCall
private constructor(
    private val raw: ZLinkStreamSendCall?,
    private val typed: ZLinkTypedStreamSendCall?,
) {
    constructor(inner: ZLinkStreamSendCall) : this(inner, null)

    constructor(inner: ZLinkTypedStreamSendCall) : this(null, inner)

    fun packetName(name: String): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(raw?.packetName(name), typed?.packetName(name))

    fun metadata(key: String, value: String): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(raw?.metadata(key, value), typed?.metadata(key, value))

    fun metadata(metadata: Map<String, String>): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(raw?.metadata(metadata), typed?.metadata(metadata))

    fun compress(): ZLinkKotlinSendCall = ZLinkKotlinSendCall(raw?.compress(), typed?.compress())

    fun submit(): CompletionStage<Void> = raw?.submit() ?: typed!!.submit()

    suspend fun await() {
        submit().await()
    }
}

suspend inline fun <reified TReply> ZLinkTypedStreamRequestCall.awaitReply(): TReply =
    submit(TReply::class.java).await()

suspend fun ZLinkStreamRequestCall.await(): ZLinkStreamEncodedPayload = submit().await()

suspend inline fun <reified TReply> ZLinkStreamRequestCall.awaitReply(): TReply =
    submit(TReply::class.java).await()

suspend inline fun <reified TPayload> ZLinkStreamWaitCall.await(): ZLinkStreamMessage<TPayload> =
    submit(TPayload::class.java).await()

inline fun <reified TPayload> ZLinkStreamConnector.waitFor(): ZLinkStreamTypedWaitCall<TPayload> =
    ZLinkStreamTypedWaitCall(waitFor(TPayload::class.java), TPayload::class.java)

class ZLinkStreamTypedWaitCall<TPayload>(
    private val inner: ZLinkStreamWaitCall,
    private val payloadType: Class<TPayload>,
) {
    fun timeout(timeout: Duration): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.timeout(timeout), payloadType)

    fun where(
        predicate: (ZLinkStreamMessage<TPayload>) -> Boolean
    ): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.where(payloadType, predicate), payloadType)

    suspend fun await(): ZLinkStreamMessage<TPayload> = inner.submit(payloadType).await()
}

class ZLinkStreamTypedExpectNoneCall<TPayload>(private val inner: ZLinkStreamExpectNoneCall) {
    fun within(window: Duration): ZLinkStreamTypedExpectNoneCall<TPayload> =
        ZLinkStreamTypedExpectNoneCall(inner.within(window))

    suspend fun await() {
        inner.submit().await()
    }
}

class ZLinkStreamTypedSequenceCall<TPayload>(
    private val inner: ZLinkStreamSequenceCall,
    private val payloadType: Class<TPayload>,
) {
    fun expect(
        predicate: (ZLinkStreamMessage<TPayload>) -> Boolean
    ): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.expect(payloadType, predicate), payloadType)

    fun timeout(timeout: Duration): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.timeout(timeout), payloadType)

    suspend fun await(): List<ZLinkStreamMessage<TPayload>> = inner.submit(payloadType).await()
}

object ZLinkKotlinStreamAssert {
    fun ensure(condition: Boolean, message: String) {
        ZLinkStreamAssert.ensure(condition, message)
    }

    suspend fun expectFailure(
        errorKind: String? = null,
        action: suspend () -> Unit,
    ): ZLinkStreamError {
        val failure = captureFailure(action)
        return ZLinkStreamAssert.expectFailure({ throw failure }, errorKind)
    }

    suspend fun expectTimeout(action: suspend () -> Unit) {
        val failure = captureFailure(action)
        ZLinkStreamAssert.expectTimeout { throw failure }
    }

    private suspend fun captureFailure(action: suspend () -> Unit): Throwable {
        try {
            action()
        } catch (error: Throwable) {
            return error
        }
        throw IllegalStateException("Expected action to fail.")
    }
}

/**
 * Carries the connector's flow context across suspension points.
 *
 * The connector keeps the current flow in a thread local (Java spec 03 7.1). A coroutine can resume
 * on a different thread than the one that suspended it, so without a [ThreadContextElement] an
 * outbound call made after a suspension point loses the inbound flow and starts a new `APPLICATION`
 * one. This element installs the captured flow on whatever thread the continuation resumes on and
 * restores that thread's previous flow when it suspends again.
 */
class ZLinkStreamFlowContextElement(private val flow: ZLinkStreamFlow?) :
    ThreadContextElement<ZLinkStreamFlowScope>, CoroutineContext.Element {
    companion object Key : CoroutineContext.Key<ZLinkStreamFlowContextElement>

    override val key: CoroutineContext.Key<ZLinkStreamFlowContextElement>
        get() = Key

    override fun updateThreadContext(context: CoroutineContext): ZLinkStreamFlowScope =
        ZLinkStreamFlowScope.enter(flow)

    override fun restoreThreadContext(context: CoroutineContext, oldState: ZLinkStreamFlowScope) {
        oldState.close()
    }
}

/**
 * The flow the calling thread currently runs under, or `null` outside an inbound handler. Pair it
 * with [ZLinkStreamFlowContextElement] to keep that flow across suspension points.
 */
fun currentZLinkStreamFlow(): ZLinkStreamFlow? = ZLinkStreamFlowScope.current()

/**
 * Runs [block] with [flow] installed as the connector flow for every thread the coroutine resumes
 * on.
 */
suspend fun <T> withZLinkStreamFlow(flow: ZLinkStreamFlow?, block: suspend () -> T): T =
    kotlinx.coroutines.withContext(ZLinkStreamFlowContextElement(flow)) { block() }

fun ZLinkStreamConnector.messages(
    packetName: String
): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> = callbackFlow {
    val registration =
        on(packetName) { message ->
            if (trySend(message).isFailure) {
                // Cancellation closes the Flow channel before the connector can
                // remove this callback. Release the message that was handed to us
                // instead of turning normal cancellation into a callback failure.
                message.payload().payload().close()
            }
            CompletableFuture.completedFuture(null)
        }
    awaitClose { registration.close() }
}

/**
 * Collects a push stream with each message's own flow installed while [action] runs. An outbound
 * call [action] makes then continues that flow instead of starting a new `APPLICATION` one, and it
 * keeps doing so across [action]'s own suspension points.
 *
 * The flow is installed around [action] rather than around the emission, because changing the
 * coroutine context between a flow's emitter and its collector is not allowed.
 */
suspend fun <TPayload> Flow<ZLinkStreamMessage<TPayload>>.collectInStreamFlow(
    action: suspend (ZLinkStreamMessage<TPayload>) -> Unit
) {
    collect { message -> withZLinkStreamFlow(message) { action(message) } }
}

fun ZLinkStreamConnector.errors(): Flow<ZLinkStreamError> = callbackFlow {
    val registration = onErrorReceived { error ->
        trySend(error)
        CompletableFuture.completedFuture(null)
    }
    awaitClose { registration.close() }
}
