// 자립형 가이드 예제: STREAM 소켓의 onPacket 콜백으로 프레임을 받아 에코한다.
//   bindings/java/gradlew -p . :kotlin-samples:runStreamPacketCallbackSample --no-daemon
package systems.zlink.samples

import systems.zlink.contracts.core.Zlink
import systems.zlink.contracts.eventing.MonitorEventType
import systems.zlink.contracts.messaging.Message
import systems.zlink.contracts.sockets.SendFlags
import java.nio.charset.StandardCharsets
import java.util.concurrent.CountDownLatch

private fun frameBytes(body: ByteArray): ByteArray {
    val frame = ByteArray(6 + body.size)
    frame[2] = (body.size ushr 24).toByte()
    frame[3] = (body.size ushr 16).toByte()
    frame[4] = (body.size ushr 8).toByte()
    frame[5] = body.size.toByte()
    System.arraycopy(body, 0, frame, 6, body.size)
    return frame
}

private fun frame(header: Message, body: Message): Message {
    val headerBytes = header.toByteArray()
    val bodyBytes = body.toByteArray()
    val frame = ByteArray(6 + headerBytes.size + bodyBytes.size)
    frame[0] = (headerBytes.size ushr 8).toByte()
    frame[1] = headerBytes.size.toByte()
    frame[2] = (bodyBytes.size ushr 24).toByte()
    frame[3] = (bodyBytes.size ushr 16).toByte()
    frame[4] = (bodyBytes.size ushr 8).toByte()
    frame[5] = bodyBytes.size.toByte()
    System.arraycopy(headerBytes, 0, frame, 6, headerBytes.size)
    System.arraycopy(bodyBytes, 0, frame, 6 + headerBytes.size, bodyBytes.size)
    return Message.from(frame)
}

fun main() {
// --8<-- [start:doc]
    SampleSupport.ensureNative()
    val endpoint = SampleSupport.tcpEndpoint()
    val delivered = CountDownLatch(1)

    Zlink.createContext().use { ctx ->
        ctx.createStreamSocket().use { server ->
            server.monitorOpen(
                MonitorEventType.ACCEPTED,
                MonitorEventType.CONNECTION_READY
            ).use { monitor ->
                server.bind(endpoint)
                SampleSupport.connectRawTcp(endpoint).use { rawClient ->
                    SampleSupport.waitStreamConnected(monitor)

                    server.onPacket { routingId, _, body ->
                        val value = body.toUtf8String()
                        if (SampleSupport.STREAM_PAYLOAD == value && routingId != null) {
                            Message.from(ByteArray(0)).use { replyHeader ->
                                Message.from(SampleSupport.STREAM_PAYLOAD).use { replyBody ->
                                    frame(replyHeader, replyBody).use { reply ->
                                        check(
                                            server.send(routingId)
                                                .message(reply)
                                                .flags(SendFlags.DONT_WAIT)
                                                .submit()
                                        ) { "stream reply backpressured" }
                                    }
                                }
                            }
                            delivered.countDown()
                        }
                    }

                    val payload = SampleSupport.STREAM_PAYLOAD.toByteArray(StandardCharsets.UTF_8)
                    val frameBytes = frameBytes(payload)
                    SampleSupport.sendRawTcp(rawClient, frameBytes)
                    SampleSupport.await(delivered, "stream packet callback")

                    val echoedFrame = SampleSupport.recvExactRawTcp(rawClient, frameBytes.size)
                    val echoed = String(
                        echoedFrame.copyOfRange(6, echoedFrame.size),
                        StandardCharsets.UTF_8
                    )
                    check(SampleSupport.STREAM_PAYLOAD == echoed) { "unexpected stream echo: $echoed" }
                    println(
                        "[stream/packet-callback] send: \"${SampleSupport.STREAM_PAYLOAD}\"" +
                            " -> recv: \"$echoed\""
                    )
                }
            }
        }
    }
// --8<-- [end:doc]
}
