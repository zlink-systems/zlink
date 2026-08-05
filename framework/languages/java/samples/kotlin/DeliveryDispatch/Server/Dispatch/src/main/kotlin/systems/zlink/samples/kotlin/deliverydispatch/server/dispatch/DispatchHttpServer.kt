package systems.zlink.samples.kotlin.deliverydispatch.server.dispatch

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CreateDeliveryReq
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.CreateDeliveryRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.DeliveryStatus
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.ServerAssertionReq

class DispatchHttpServer(
    private val json: ObjectMapper,
    private val queue: DispatchWorkQueue,
) : AutoCloseable {
    private val server: HttpServer

    init {
        val endpoint = URI.create(SampleTopology.DispatchHttpEndpoint)
        server = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0)
        server.createContext("/deliveries", ::handleCreateDelivery)
        server.createContext("/self-check/assert", ::handleServerAssertion)
        server.start()
    }

    private fun handleCreateDelivery(exchange: HttpExchange) {
        if (exchange.requestMethod != "POST") {
            exchange.writeJson(405, "")
            return
        }
        val request = json.readValue(exchange.requestBody, CreateDeliveryReq::class.java)
        queue.enqueue(request)
        exchange.writeJson(
            200,
            json.writeValueAsString(CreateDeliveryRes(request.deliveryId)),
        )
    }

    private fun handleServerAssertion(exchange: HttpExchange) {
        if (exchange.requestMethod != "POST") {
            exchange.writeJson(405, "")
            return
        }
        val request = json.readValue(exchange.requestBody, ServerAssertionReq::class.java)
        val response = queue.assertServerEvidence(request)
        exchange.writeJson(if (response.passed) 200 else 500, json.writeValueAsString(response))
    }

    private fun HttpExchange.writeJson(status: Int, body: String) {
        val bytes = body.toByteArray(StandardCharsets.UTF_8)
        responseHeaders.add("content-type", "application/json")
        sendResponseHeaders(status, bytes.size.toLong())
        responseBody.use { it.write(bytes) }
        close()
    }

    override fun close() {
        server.stop(0)
    }
}
