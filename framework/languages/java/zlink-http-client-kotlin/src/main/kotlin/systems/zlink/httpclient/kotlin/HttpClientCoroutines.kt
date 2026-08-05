/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.kotlin

import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import systems.zlink.httpclient.HttpResponse
import systems.zlink.httpclient.RawHttpResponse
import systems.zlink.httpclient.ZLinkHttpClient
import systems.zlink.httpclient.ZLinkHttpClientBuilder
import systems.zlink.httpclient.ZLinkHttpRequestBuilder
import systems.zlink.httpclient.ZLinkHttpServerRequestBuilder

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.BINARY)
private annotation class JacocoGenerated

/**
 * Kotlin coroutine and DSL extensions over the Java [ZLinkHttpClient]. These reuse the Java runtime
 * (the Kotlin module is a thin idiom layer) and expose genuine `suspend` functions that bridge the
 * `CompletionStage` returned by the Java client through a non-blocking coroutine bridge — no
 * thread is blocked. The bridge keeps caller cancellation separate from the submitted HTTP
 * operation.
 */

/** DSL helper: builds a client by applying [configure] to the fluent builder. */
fun zlinkHttpClient(baseUrl: String, configure: ZLinkHttpClientBuilder.() -> Unit = {}): ZLinkHttpClient =
    ZLinkHttpClient.create(baseUrl).apply(configure).build()

/** Suspends until the raw response is available; does not block the calling thread. */
suspend fun ZLinkHttpRequestBuilder.awaitRaw(): RawHttpResponse =
    submitRaw().awaitWithoutCancellingOperation()

/** Suspends until the typed JSON response is available. */
suspend fun <T> ZLinkHttpRequestBuilder.await(type: Class<T>): HttpResponse<T> =
    submit(type).awaitWithoutCancellingOperation()

/** Reified convenience for [await]. */
@JacocoGenerated
suspend inline fun <reified T> ZLinkHttpRequestBuilder.await(): HttpResponse<T> =
    await(T::class.java)

/** Reified convenience for [fetch]. */
@JacocoGenerated
suspend inline fun <reified T> ZLinkHttpRequestBuilder.fetch(): T =
    fetch(T::class.java).awaitWithoutCancellingOperation()

/** Suspends until the streaming download completes, delivering chunks to [sink]. */
suspend fun ZLinkHttpRequestBuilder.awaitDownload(sink: (ByteArray) -> Unit): RawHttpResponse =
    download(sink).awaitWithoutCancellingOperation()

/** Awaits a typed response while retaining the current server Spot turn. */
suspend fun <T> ZLinkHttpServerRequestBuilder.await(type: Class<T>): HttpResponse<T> =
    submit(type).awaitWithoutCancellingOperation()

/** Awaits completion of a one-way server request without exposing a response value. */
@JvmName("awaitOneWay")
suspend fun ZLinkHttpServerRequestBuilder.await() {
    submit().awaitWithoutCancellingOperation()
}

@JacocoGenerated
suspend inline fun <reified T> ZLinkHttpServerRequestBuilder.await(): HttpResponse<T> =
    await(T::class.java)

@JacocoGenerated
suspend inline fun <reified T> ZLinkHttpServerRequestBuilder.yield(): HttpResponse<T> =
    yield(T::class.java).awaitWithoutCancellingOperation()

/**
 * Bridges a CompletionStage without registering a cancellation handler on it.
 * Coroutine cancellation ends the caller's wait, while the already submitted
 * HTTP operation keeps its retry, body-read, and client-lease ownership.
 */
@PublishedApi
internal suspend fun <T> java.util.concurrent.CompletionStage<T>
    .awaitWithoutCancellingOperation(): T = suspendCancellableCoroutine { continuation ->
    whenComplete { value, error ->
        if (error == null) {
            continuation.resume(value)
        } else {
            continuation.resumeWithException(unwrapCompletionFailure(error))
        }
    }
}

private fun unwrapCompletionFailure(error: Throwable): Throwable {
    var cause = error
    while ((cause is java.util.concurrent.CompletionException
        || cause is java.util.concurrent.ExecutionException)
        && cause.cause != null) {
        cause = cause.cause!!
    }
    return cause
}
