package systems.zlink.e2e.kotlin.spotservice.gateway

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.util.concurrent.ConcurrentLinkedQueue
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.SmartLifecycle
import org.springframework.context.annotation.Bean
import org.springframework.core.env.Environment
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.Env
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.spots.ZLinkSpotPublisherClient
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.spotservice.gateway"]
)
class GatewayApplication {
    @Bean
    fun gatewayOptions(environment: Environment): GatewayOptions =
        GatewayOptions.fromSpringEnvironment(environment)

    @Bean
    fun evidenceStore(options: GatewayOptions): GatewayEvidenceStore =
        GatewayEvidenceStore(options.rid, options.evidenceFile)

    @Bean
    fun objectMapper(): ObjectMapper = ObjectMapper()

    @Bean
    fun gatewayHttpServer(
        options: GatewayOptions,
        json: ObjectMapper,
        publisher: ZLinkSpotPublisherClient,
        evidence: GatewayEvidenceStore,
        context: ConfigurableApplicationContext
    ): GatewayHttpServer =
        GatewayHttpServer(options, json, publisher, evidence, context)

    @Bean
    fun gatewayFramework(options: GatewayOptions): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { framework ->
            Files.createDirectories(Path.of(options.logDir))
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${options.logDir}/${options.rid}-flow.log")
                .traceLabel(options.rid)
            val mesh = framework.addRouteMesh(Contracts.SPOT_MESH)
                .listen(requireOption(options.spotPubEndpoint, "--spot-pub-endpoint"))
                .setRoutingId(RoutingId.from(options.rid))
            mesh.channelName(Contracts.ROUTE_CHANNEL).client()
            mesh.configureSpotPublisher()
        }

    @Bean
    fun locationStore(): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(Env.get("e2e.redis.location.endpoint"))
                .setKeyPrefix(Env.get("e2e.location.key.prefix"))
        )

    companion object {
        @JvmStatic
        fun run(vararg args: String): AutoCloseable {
            Env.configure(args)
            val options = GatewayOptions.parse(args)
            val builder = SpringApplicationBuilder(GatewayApplication::class.java)
                .web(WebApplicationType.NONE)
                .properties(options.toProperties())
            builder.application().setKeepAlive(true)
            val context = builder.run(*Env.applicationArgs(args))
            return AutoCloseable { context.close() }
        }

        private fun requireOption(value: String, optionName: String): String {
            require(value.isNotBlank()) { "$optionName is required." }
            return value
        }
    }
}

class GatewayHttpServer(
    private val options: GatewayOptions,
    private val json: ObjectMapper,
    private val publisher: ZLinkSpotPublisherClient,
    private val evidence: GatewayEvidenceStore,
    private val context: ConfigurableApplicationContext
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var running = false

    override fun start() {
        if (options.httpUrl.isBlank()) {
            return
        }
        val uri = URI.create(options.httpUrl)
        val host = if (uri.host.isNullOrBlank()) "127.0.0.1" else uri.host
        val nextServer = HttpServer.create(InetSocketAddress(host, uri.port), 0)
        nextServer.createContext("/health") { exchange ->
            writeJson(exchange, 200, mapOf("status" to "ready", "rid" to options.rid))
        }
        nextServer.createContext("/evidence") { exchange ->
            writeJson(exchange, 200, evidence.snapshot())
        }
        nextServer.createContext("/spot/publish") { exchange ->
            if (exchange.requestMethod != "POST") {
                write(exchange, 405, "method not allowed\n")
                return@createContext
            }
            val request = readPublishRequest(exchange)
            publisher.publish(
                Contracts.ROUTE_CHANNEL,
                "spot.events",
                Contracts.MeshMsg(request.marker)
            )
                .submit()
            evidence.add("spot-publish|rid=${options.rid}|spot=${request.spotRid}|marker=${request.marker}")
            writeJson(
                exchange,
                200,
                mapOf(
                    "scenario" to "spot.sm-c4-publish",
                    "rid" to options.rid,
                    "spotRid" to request.spotRid,
                    "marker" to request.marker,
                    "evidence" to evidence.snapshot()
                )
            )
        }
        nextServer.createContext("/shutdown") { exchange ->
            writeJson(exchange, 200, mapOf("status" to "stopping"))
            Thread { context.close() }.start()
        }
        nextServer.start()
        server = nextServer
        running = true
    }

    override fun stop() {
        server?.stop(0)
        server = null
        running = false
    }

    override fun isRunning(): Boolean =
        running

    private fun readPublishRequest(exchange: HttpExchange): SpotPublishReq {
        val body = exchange.requestBody.readBytes()
        val value = json.readValue(body, Map::class.java)
        val marker = value["marker"]?.toString().orEmpty()
        require(marker.isNotBlank()) { "marker is required." }
        val spotRid = value["spotRid"]?.toString().orEmpty()
        return SpotPublishReq(marker, spotRid)
    }

    private fun writeJson(exchange: HttpExchange, status: Int, value: Any) {
        val body = json.writeValueAsBytes(value)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }

    private fun write(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }
}

data class SpotPublishReq(
    val marker: String,
    val spotRid: String
)

class GatewayEvidenceStore(
    rid: String,
    private val evidenceFile: String
) {
    private val entries = ConcurrentLinkedQueue<String>()

    init {
        add("start|rid=$rid")
    }

    fun add(value: String) {
        entries.add(value)
        if (evidenceFile.isNotBlank()) {
            Files.writeString(
                Path.of(evidenceFile),
                "$value\n",
                StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND
            )
        }
    }

    fun snapshot(): List<String> =
        entries.toList()
}

data class GatewayOptions(
    val rid: String,
    val httpUrl: String,
    val logDir: String,
    val evidenceFile: String,
    val spotPubEndpoint: String
) {
    fun toProperties(): Map<String, Any> =
        mapOf(
            "zlink.e2e.gateway.rid" to rid,
            "zlink.e2e.gateway.http-url" to httpUrl,
            "zlink.e2e.gateway.log-dir" to logDir,
            "zlink.e2e.gateway.evidence-file" to evidenceFile,
            "zlink.e2e.gateway.spot-pub-endpoint" to spotPubEndpoint
        )

    companion object {
        fun parse(args: Array<out String>): GatewayOptions {
            val values = mutableMapOf<String, String>()
            var index = 0
            while (index < args.size) {
                val key = args[index]
                if (!key.startsWith("--")) {
                    index++
                    continue
                }
                require(index + 1 < args.size) { "Missing value for $key." }
                values[key.removePrefix("--")] = args[index + 1]
                index += 2
            }
            return GatewayOptions(
                rid = values["rid"] ?: Env.get("e2e.gateway.rid", "gateway"),
                httpUrl = values["http-url"] ?: Env.get("e2e.gateway.http.url"),
                logDir = values["log-dir"] ?: Env.get("e2e.log.dir", "logs"),
                evidenceFile = values["evidence-file"] ?: Env.get("e2e.gateway.evidence.file"),
                spotPubEndpoint = values["spot-pub-endpoint"] ?: Env.get("e2e.gateway.spot.pub.endpoint")
            )
        }

        fun fromSpringEnvironment(environment: Environment): GatewayOptions =
            GatewayOptions(
                rid = environment.getProperty("zlink.e2e.gateway.rid", ""),
                httpUrl = environment.getProperty("zlink.e2e.gateway.http-url", ""),
                logDir = environment.getProperty("zlink.e2e.gateway.log-dir", ""),
                evidenceFile = environment.getProperty("zlink.e2e.gateway.evidence-file", ""),
                spotPubEndpoint = environment.getProperty("zlink.e2e.gateway.spot-pub-endpoint", "")
            )
    }
}
