// 자립형 가이드 예제: DEALER → ROUTER 비동기 요청/응답 (CompletableFuture).
//   bindings/java/gradlew -p . :kotlin-samples:runRequestReplyAsyncSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.RoutingId
import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.messaging.Received
import systems.zlink.contracts.sockets.RecvFlags
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

fun main() {
// --8<-- [start:doc]
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()
    val requestHandled = CountDownLatch(1)

    Zlink.createContext().use { ctx ->
        ctx.createRouterSocket().use { routerSocket ->
            ctx.createDealerSocket().use { dealerSocket ->
                routerSocket.monitorOpen(MonitorEventType.CONNECTION_READY).use { routerMonitor ->
                    dealerSocket.monitorOpen(MonitorEventType.CONNECTION_READY).use { dealerMonitor ->
                        dealerSocket.setRoutingId(RoutingId.from("request-reply-client".toByteArray()))
                        routerSocket.bind(endpoint)
                        dealerSocket.connect(endpoint)
                        SampleSupport.waitConnected(routerMonitor, dealerMonitor)

                        val replyHandled = CompletableFuture<Void>()

                        val routerThread = Thread({
                            try {
                                Received().use { received ->
                                    routerSocket.recv(received, RecvFlags.NONE)
                                    val request = SampleSupport.singleUtf8(received)
                                    check(SampleSupport.DEALER_REQUEST == request) { "unexpected request: $request" }
                                    check(!received.requestSeq().isEmpty) { "missing request sequence" }
                                    Message.from(SampleSupport.DEALER_REPLY).use { reply ->
                                        received.reply().message(reply).submit()
                                    }
                                    requestHandled.countDown()
                                    replyHandled.complete(null)
                                }
                            } catch (error: Throwable) {
                                replyHandled.completeExceptionally(error)
                                throw error
                            }
                        }, "request-reply-async-router")
                        routerThread.start()

                        val roundTrip: java.util.concurrent.CompletionStage<Void>
                        Message.from(SampleSupport.DEALER_REQUEST).use { request ->
                            roundTrip = dealerSocket.request()
                                .message(request)
                                .timeout(Duration.ofSeconds(2))
                                .submit()
                                .thenAccept { reply ->
                                    try {
                                        val value = reply[0].toUtf8String()
                                        check(SampleSupport.DEALER_REPLY == value) { "unexpected reply: $value" }
                                    } finally {
                                        Message.closeAll(reply)
                                    }
                                }
                        }

                        roundTrip.toCompletableFuture().get(2, TimeUnit.SECONDS)
                        replyHandled.get(2, TimeUnit.SECONDS)
                        SampleSupport.await(requestHandled, "request reply async")
                        println(
                            "[dealer-router/request-reply/async] send: \"${SampleSupport.DEALER_REQUEST}\"" +
                                " -> recv: \"${SampleSupport.DEALER_REPLY}\""
                        )
                    }
                }
            }
        }
    }
// --8<-- [end:doc]
}
