/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

import com.google.protobuf.ByteString
import java.time.Duration
import kotlinx.coroutines.CoroutineScope
import org.springframework.boot.Banner
import org.springframework.boot.WebApplicationType
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.ConfigurableApplicationContext
import systems.zlink.bench.withgrpc.client.BenchOperation
import systems.zlink.bench.withgrpc.client.BenchOptions
import systems.zlink.bench.withgrpc.client.FrameworkStack
import systems.zlink.bench.withgrpc.proto.BenchPayload
import systems.zlink.bench.withgrpc.shared.BenchContract
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply

/**
 * `zlink-framework-kotlin` client: RouteMesh channel messaging through the suspend
 * interface of `zlink-framework-kotlin`.
 *
 * <p>spec section 8.1: the framework module is `zlink-framework-kotlin`, stood up through
 * its public host -- the Spring Boot starter -- with the same protobuf codec the java
 * row uses. No internal package is touched (G4).
 *
 * <p>The host configuration is the java row's, reused verbatim: the same
 * `@EnableZLinkFramework` application class, the same RouteMesh client channel, the same
 * manual peer connection. Rewriting it in kotlin would have changed the wiring as well
 * as the call API, and then a difference between the two rows could not be attributed.
 * What differs is the call itself: `awaitReply` and `await` from
 * `zlink-framework-kotlin`, not `CompletionStage`.
 */
class FrameworkKotlinStack private constructor(
    private val context: ConfigurableApplicationContext,
    private val route: ZLinkRouteClient,
    private val runId: Int,
    private val timeout: Duration,
    private val scope: CoroutineScope,
) : AutoCloseable {

    companion object {
        fun create(options: BenchOptions, scope: CoroutineScope): FrameworkKotlinStack {
            val context = SpringApplicationBuilder(
                FrameworkStack.BenchClientFrameworkApp::class.java,
            )
                .web(WebApplicationType.NONE)
                .bannerMode(Banner.Mode.OFF)
                .initializers({ applicationContext ->
                    applicationContext.beanFactory.registerSingleton(
                        "benchPeerEndpoint",
                        FrameworkStack.PeerEndpoint(options.zlinkEndpoint),
                    )
                })
                .run()
            val route = context.getBean(ZLinkRouteClient::class.java)
            return FrameworkKotlinStack(
                context,
                route,
                options.runId,
                Duration.ofMillis(options.requestTimeoutMs.toLong()),
                scope,
            )
        }
    }

    fun request(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            val reply: BenchPayload = route
                .requestToChannel(BenchContract.CHANNEL_NAME, payload(payloadSize, phase, sequence))
                .timeout(timeout)
                .awaitReply(BenchPayload::class.java)
            val decoded = BenchMetricHeader.decode(reply.body.asReadOnlyByteBuffer())
            check(
                BenchMetricHeader.isExpected(decoded, runId, phase, payloadSize, sequence),
            ) { "framework reply header mismatch" }
        }
    }

    fun send(): BenchOperation = BenchOperation { payloadSize, phase, sequence ->
        scope.benchFuture {
            route
                .sendToChannel(BenchContract.CHANNEL_NAME, payload(payloadSize, phase, sequence))
                .submit()
                .await()
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
        context.close()
    }
}
