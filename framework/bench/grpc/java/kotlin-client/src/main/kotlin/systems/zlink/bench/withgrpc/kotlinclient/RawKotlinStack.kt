/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import java.nio.charset.StandardCharsets
import java.time.Duration
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.future.await
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader
import systems.zlink.bench.withgrpc.shared.RawWire
import systems.zlink.contracts.core.Context
import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.sockets.DealerSocket
import systems.zlink.contracts.sockets.RouterSocket
import systems.zlink.contracts.sockets.Socket

/**
 * `zlink-kotlin` client: the raw binding, ROUTER&lt;-&gt;ROUTER by default.
 *
 * <p>`bindings/kotlin` holds no native binding of its own -- it is the java binding
 * artifact `systems.zlink:zlink` used from kotlin, and its own README says so. So this
 * row measures the kotlin-facing use of that binding: the request is awaited with
 * `kotlinx.coroutines.future.await` inside a coroutine, the shape the kotlin binding
 * samples use, rather than a blocking `get()`.
 *
 * <p>FB-001 / spec section 1.3: ROUTER&lt;-&gt;ROUTER, so that
 * `zlink-framework-kotlin / zlink-kotlin` isolates framework-layer cost instead of
 * mixing in a DEALER-&gt;ROUTER socket-pattern difference. DEALER exists only for the
 * one comparison run the campaign keeps beside the three ROUTER runs.
 */
class RawKotlinStack private constructor(
    private val socket: Socket,
    private val router: RouterSocket?,
    private val dealer: DealerSocket?,
    private val peer: RoutingId,
    private val runId: Int,
    private val timeout: Duration,
    private val scope: CoroutineScope,
) : AutoCloseable {

    companion object {
        fun create(
            context: Context,
            options: BenchOptions,
            selfId: String,
            peerId: String,
            endpoint: String,
            scope: CoroutineScope,
        ): RawKotlinStack {
            val peer = RoutingId.from(peerId.toByteArray(StandardCharsets.US_ASCII))
            val self = RoutingId.from(selfId.toByteArray(StandardCharsets.US_ASCII))
            val timeout = Duration.ofMillis(options.requestTimeoutMs.toLong())
            if (options.rawSocket == "dealer") {
                val dealer = context.createDealerSocket()
                dealer.setRoutingId(self)
                dealer.connect(endpoint)
                return RawKotlinStack(
                    dealer, null, dealer, peer, options.runId, timeout, scope,
                )
            }
            val router = context.createRouterSocket()
            router.setRoutingId(self)
            router.options().mandatory(true)
            router.options().setConnectRoutingId(peer)
            router.connect(endpoint)
            return RawKotlinStack(router, router, null, peer, options.runId, timeout, scope)
        }
    }

    fun request(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            val payload =
                BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)
            val call = if (router != null) router.request(peer) else dealer!!.request()
            // FB-024: an envelope header part plus a protobuf-encoded BenchPayload part.
            // The same two parts zlink-c and zlink-java put on the wire; formula 1
            // divides zlink-<lang> by zlink-c, so a different wire shape would divide
            // two different experiments.
            val parts = call
                .message(Message.from(RawWire.REQUEST_ENVELOPE))
                .message(Message.from(RawWire.encodeBenchPayload(payload)))
                .timeout(timeout)
                .submit()
                .toCompletableFuture()
                .await()
            try {
                check(parts.isNotEmpty()) { "raw request returned no reply parts" }
                val body = RawWire.decodeBenchPayloadBody(parts.last().dataBuffer())
                val decoded = BenchMetricHeader.decode(body)
                check(
                    BenchMetricHeader.isExpected(
                        decoded, runId, phase, payloadSize, sequence,
                    ),
                ) { "raw reply header mismatch" }
            } finally {
                parts.forEach { it.close() }
            }
        }
    }

    fun send(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            val payload =
                BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence)
            val call = if (router != null) router.send(peer) else dealer!!.send()
            call
                .message(Message.from(RawWire.REQUEST_ENVELOPE))
                .message(Message.from(RawWire.encodeBenchPayload(payload)))
                .submit()
                .toCompletableFuture()
                .await()
        }
    }

    override fun close() {
        socket.close()
    }
}
