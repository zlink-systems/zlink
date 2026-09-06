/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import com.google.protobuf.ByteString
import io.grpc.ManagedChannel
import io.grpc.ManagedChannelBuilder
import kotlinx.coroutines.CoroutineScope
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.proto.BenchPayload
import systems.zlink.bench.withgrpc.proto.BenchServiceGrpcKt
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader

/**
 * `grpc-kotlin` client: the grpc-kotlin coroutine stub, default channel configuration.
 *
 * <p>spec section 8.1 requires the coroutine stub for kotlin and allows the grpc-java
 * blocking stub only when the coroutine stub cannot be used, with the reason recorded.
 * It can be used here, so it is: `BenchServiceCoroutineStub.echo` and `.command` are
 * suspend functions, the same shape as the suspend calls the ZLink rows make.
 *
 * <p>The server is the java row's grpc-netty-shaded server on the kotlin port band. gRPC
 * is language-neutral on the wire and spec section 8.2 asks for the server configuration to
 * be recorded, not for it to be rewritten per language.
 */
class GrpcKotlinStack(options: BenchOptions, private val scope: CoroutineScope) :
    AutoCloseable {

    private val channel: ManagedChannel = ManagedChannelBuilder.forTarget(options.grpcUrl)
        .usePlaintext()
        .build()
    private val stub = BenchServiceGrpcKt.BenchServiceCoroutineStub(channel)
    private val runId = options.runId

    fun echo(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            val reply = stub.echo(payload(payloadSize, phase, sequence))
            // G2: the reply's 29-byte header is validated, not assumed.
            val decoded = BenchMetricHeader.decode(reply.body.asReadOnlyByteBuffer())
            check(
                BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence),
            ) { "grpc-kotlin echo reply header mismatch" }
        }
    }

    fun command(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            // FB-002: the send comparison uses this unary Command returning Empty. gRPC
            // has no one-way primitive, so it pays a round trip for a command that needs
            // no reply; in a service-side comparison that cost stays in the result.
            stub.command(payload(payloadSize, phase, sequence))
        }
    }

    private fun payload(payloadSize: Int, phase: Byte, sequence: Long): BenchPayload =
        BenchPayload.newBuilder()
            .setBody(
                ByteString.copyFrom(
                    BenchMetricHeader.createPayload(payloadSize, runId, phase, sequence),
                ),
            )
            .build()

    override fun close() {
        channel.shutdownNow()
    }
}
