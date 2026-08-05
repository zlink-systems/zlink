package systems.zlink.framework.kotlin

import kotlinx.coroutines.future.await
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import java.time.Duration
import java.util.concurrent.CompletionStage
import systems.zlink.stream.connector.ZLinkStreamConnector
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload
import systems.zlink.stream.connector.ZLinkStreamError
import systems.zlink.stream.connector.ZLinkStreamLifecycleCall
import systems.zlink.stream.connector.ZLinkStreamMessage
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamCompressionCodec
import systems.zlink.stream.connector.ZLinkStreamCompressionCodecs
import systems.zlink.stream.connector.ZLinkStreamRequestCall
import systems.zlink.stream.connector.ZLinkStreamSendCall
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamWaitCall
import systems.zlink.stream.connector.ZLinkTypedStreamRequestCall
import systems.zlink.stream.connector.ZLinkTypedStreamSendCall
import systems.zlink.stream.connector.ZLinkStreamConnectionState
import systems.zlink.stream.connector.ZLinkStreamConnectionStateHandler
import systems.zlink.stream.connector.ZLinkStreamDisconnectedHandler
import systems.zlink.stream.connector.ZLinkStreamErrorHandler
import systems.zlink.stream.connector.ZLinkStreamExpectNoneCall
import systems.zlink.stream.connector.ZLinkStreamInboundObserver
import systems.zlink.stream.connector.ZLinkStreamMessageHandler
import systems.zlink.stream.connector.ZLinkStreamSequenceCall
import systems.zlink.stream.connector.ZLinkStreamAssert

fun ZLinkStreamConnector.kotlin(): ZLinkKotlinStreamConnector =
    ZLinkKotlinStreamConnector(this)

fun ZLinkStreamConnectorOptions.withDefaultStreamCompression(): ZLinkStreamConnectorOptions =
    withStreamCompression(ZLinkStreamCompressionCodecs.lz4())

fun ZLinkStreamConnectorOptions.withLz4StreamCompression(): ZLinkStreamConnectorOptions =
    withDefaultStreamCompression()

fun ZLinkStreamConnectorOptions.withStreamCompression(
    codec: ZLinkStreamCompressionCodec,
): ZLinkStreamConnectorOptions =
    copyStreamCompression(ZLinkStreamCompression.LZ4, codec)

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
        maxReceivedMessages(),
        maxInboundObserverNotifications(),
        maxInboundObserverPayloadPreviewBytes(),
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
    )

class ZLinkKotlinStreamConnector(
    @PublishedApi
    internal val inner: ZLinkStreamConnector,
) {
    val isConnected: Boolean
        get() = inner.isConnected

    val state: ZLinkStreamConnectionState
        get() = inner.state()

    val options: ZLinkStreamConnectorOptions
        get() = inner.options()

    val pendingDispatchCount: Int
        get() = inner.pendingDispatchCount()

    fun receivedCount(name: String): Int =
        inner.receivedCount(name)

    fun observeInbound(observer: ZLinkStreamInboundObserver): AutoCloseable =
        inner.observeInbound(observer)

    fun on(
        name: String,
        handler: ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>,
    ): AutoCloseable = inner.on(name, handler)

    inline fun <reified TPayload> on(
        handler: ZLinkStreamMessageHandler<TPayload>,
    ): AutoCloseable = inner.on(TPayload::class.java, handler)

    fun onErrorReceived(handler: ZLinkStreamErrorHandler): AutoCloseable =
        inner.onErrorReceived(handler)

    fun onDisconnected(handler: ZLinkStreamDisconnectedHandler): AutoCloseable =
        inner.onDisconnected(handler)

    fun onConnectionStateChanged(handler: ZLinkStreamConnectionStateHandler): AutoCloseable =
        inner.onConnectionStateChanged(handler)

    fun connect(): ZLinkKotlinLifecycleCall =
        ZLinkKotlinLifecycleCall(inner.connect())

    fun close(): ZLinkKotlinLifecycleCall =
        ZLinkKotlinLifecycleCall(inner.close())

    fun dispatch(): ZLinkKotlinLifecycleCall =
        ZLinkKotlinLifecycleCall(inner.dispatch())

    fun send(payload: ZLinkStreamEncodedPayload): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(inner.send(payload))

    fun send(payload: Any): ZLinkKotlinSendCall =
        ZLinkKotlinSendCall(inner.send(payload))

    fun request(payload: ZLinkStreamEncodedPayload): ZLinkStreamRequestCall =
        inner.request(payload)

    fun request(payload: Any): ZLinkTypedStreamRequestCall =
        inner.request(payload)

    inline fun <reified TPayload> waitFor(): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.waitFor(TPayload::class.java), TPayload::class.java)

    inline fun <reified TPayload> waitFor(name: String): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.waitFor(name), TPayload::class.java)

    inline fun <reified TPayload> expectNone(name: String): ZLinkStreamTypedExpectNoneCall<TPayload> =
        ZLinkStreamTypedExpectNoneCall(inner.expectNone(name))

    inline fun <reified TPayload> waitForSequence(name: String): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.waitForSequence(name), TPayload::class.java)

    fun messages(packetName: String): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> =
        inner.messages(packetName)

    fun errors(): Flow<ZLinkStreamError> =
        inner.errors()
}

class ZLinkKotlinLifecycleCall(
    private val inner: ZLinkStreamLifecycleCall,
) {
    suspend fun await() {
        inner.submit().await()
    }
}

class ZLinkKotlinSendCall(
    private val submitter: () -> CompletionStage<Void>,
) {
    constructor(inner: ZLinkStreamSendCall) : this({ inner.submit() })

    constructor(inner: ZLinkTypedStreamSendCall) : this({ inner.submit() })

    suspend fun await() {
        submitter().await()
    }
}

suspend inline fun <reified TReply> ZLinkTypedStreamRequestCall.awaitReply(): TReply =
    submit(TReply::class.java).await()

suspend fun ZLinkStreamRequestCall.await(): ZLinkStreamEncodedPayload =
    submit().await()

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
        predicate: (ZLinkStreamMessage<TPayload>) -> Boolean,
    ): ZLinkStreamTypedWaitCall<TPayload> =
        ZLinkStreamTypedWaitCall(inner.where(payloadType, predicate), payloadType)

    suspend fun await(): ZLinkStreamMessage<TPayload> =
        inner.submit(payloadType).await()
}

class ZLinkStreamTypedExpectNoneCall<TPayload>(
    private val inner: ZLinkStreamExpectNoneCall,
) {
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
        predicate: (ZLinkStreamMessage<TPayload>) -> Boolean,
    ): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.expect(payloadType, predicate), payloadType)

    fun timeout(timeout: Duration): ZLinkStreamTypedSequenceCall<TPayload> =
        ZLinkStreamTypedSequenceCall(inner.timeout(timeout), payloadType)

    suspend fun await(): List<ZLinkStreamMessage<TPayload>> =
        inner.submit(payloadType).await()
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

fun ZLinkStreamConnector.messages(
    packetName: String,
): Flow<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> = callbackFlow {
    val registration = on(packetName) { message ->
        trySend(message).getOrThrow()
        java.util.concurrent.CompletableFuture.completedFuture(null)
    }
    awaitClose {
        registration.close()
    }
}

fun ZLinkStreamConnector.errors(): Flow<ZLinkStreamError> = callbackFlow {
    val registration = onErrorReceived { error ->
        trySend(error).getOrThrow()
        java.util.concurrent.CompletableFuture.completedFuture(null)
    }
    awaitClose {
        registration.close()
    }
}
