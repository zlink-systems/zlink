package systems.zlink.e2e.kotlin.registrationcodec.main.endpoints

import com.fasterxml.jackson.databind.ObjectMapper
import com.google.protobuf.StringValue
import com.sun.net.httpserver.HttpExchange
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import systems.zlink.e2e.kotlin.registrationcodec.CodecRoundtripRes
import systems.zlink.e2e.kotlin.registrationcodec.Contracts
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleReq
import systems.zlink.e2e.kotlin.registrationcodec.DiLifecycleRes
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoAttrRes
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoAutoRes
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualMsg
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualReq
import systems.zlink.e2e.kotlin.registrationcodec.EchoManualRes
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.JsonEchoRes
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoMsg
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoReq
import systems.zlink.e2e.kotlin.registrationcodec.PackedEchoRes
import systems.zlink.framework.channels.ZLinkClient

class RegistrationScenarioEndpoints(
    private val client: ZLinkClient,
    private val json: ObjectMapper,
) {
    fun map(httpServer: com.sun.net.httpserver.HttpServer) {
        httpServer.createContext("/registration/auto") { exchange ->
            exchange.writeJson(request(EchoAutoReq("auto-request"), EchoAutoRes::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, EchoAutoMsg("auto-send")).submit()
                reply
            })
        }
        httpServer.createContext("/registration/attribute") { exchange ->
            exchange.writeJson(request(EchoAttrReq("attr-request"), EchoAttrRes::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, EchoAttrMsg("attr-send")).submit()
                reply
            })
        }
        httpServer.createContext("/registration/manual") { exchange ->
            exchange.writeJson(request(EchoManualReq("manual-request"), EchoManualRes::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, EchoManualMsg("manual-send")).submit()
                reply
            })
        }
        httpServer.createContext("/registration/di-lifecycle") { exchange ->
            val replies = mutableListOf<DiLifecycleRes>()
            var sequence: CompletionStage<Void> = CompletableFuture.completedFuture(null)
            repeat(3) { index ->
                sequence = sequence.thenCompose {
                    request(DiLifecycleReq("di-$index"), DiLifecycleRes::class.java)
                        .thenAccept(replies::add)
                }
            }
            exchange.writeJson(sequence.thenApply { replies.toList() })
        }
        httpServer.createContext("/registration/filter-order") { exchange ->
            exchange.writeJson(request(EchoManualReq("filter-order-request"), EchoManualRes::class.java))
        }
        httpServer.createContext("/codec/json") { exchange ->
            exchange.writeJson(request(JsonEchoReq("json-request"), JsonEchoRes::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, JsonEchoMsg("json-send")).submit()
                reply
            })
        }
        httpServer.createContext("/codec/protobuf") { exchange ->
            exchange.writeJson(request(StringValue.of("protobuf-request"), StringValue::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, StringValue.of("protobuf-send")).submit()
                mapOf("value" to reply.value)
            })
        }
        httpServer.createContext("/codec/messagepack") { exchange ->
            exchange.writeJson(request(PackedEchoReq("msgpack-request"), PackedEchoRes::class.java).thenApply { reply ->
                client.sendToChannel(Contracts.CHANNEL, PackedEchoMsg("msgpack-send")).submit()
                reply
            })
        }
        httpServer.createContext("/codec/roundtrip") { exchange ->
            val response = request(JsonEchoReq("json-request"), JsonEchoRes::class.java)
                .thenCompose { jsonReply ->
                    request(StringValue.of("protobuf-request"), StringValue::class.java)
                        .thenApply { protobufReply -> jsonReply to protobufReply }
                }
                .thenCompose { (jsonReply, protobufReply) ->
                    request(PackedEchoReq("msgpack-request"), PackedEchoRes::class.java)
                        .thenApply { packedReply ->
                            CodecRoundtripRes(jsonReply.value, protobufReply.value, packedReply.value)
                        }
                }
            exchange.writeJson(response)
        }
    }

    private fun <TReply> request(payload: Any, replyType: Class<TReply>): CompletionStage<TReply> =
        client.requestToChannel(Contracts.CHANNEL, payload)
            .timeout(Duration.ofSeconds(5))
            .submit(replyType)

    private fun HttpExchange.writeJson(response: CompletionStage<*>) {
        response.whenComplete { value, error ->
            if (error == null) {
                writeJsonValue(value)
            } else {
                val cause = if (error is CompletionException && error.cause != null) error.cause!! else error
                val body = json.writeValueAsBytes(mapOf("error" to (cause.message ?: cause.javaClass.simpleName)))
                responseHeaders.add("Content-Type", "application/json")
                sendResponseHeaders(500, body.size.toLong())
                responseBody.use { it.write(body) }
                close()
            }
        }
    }

    private fun HttpExchange.writeJsonValue(value: Any?) {
        val body = json.writeValueAsBytes(value)
        responseHeaders.add("Content-Type", "application/json")
        sendResponseHeaders(200, body.size.toLong())
        responseBody.use { it.write(body) }
        close()
    }
}
