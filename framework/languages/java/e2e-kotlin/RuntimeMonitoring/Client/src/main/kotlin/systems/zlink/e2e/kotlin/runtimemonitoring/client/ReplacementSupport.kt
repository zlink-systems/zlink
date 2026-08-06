package systems.zlink.e2e.kotlin.runtimemonitoring.client

import java.net.Socket
import java.net.URI
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import systems.zlink.e2e.kotlin.runtimemonitoring.Env

class ReplacementSupport(private val options: ClientOptions) {
    fun startFiltered(): Process {
        val config = Path.of(options.logDir, "filtered-service-replacement.properties")
        Files.writeString(
            config,
            listOf(
                "e2e.rid=svc-b",
                "e2e.redis.location.endpoint=${Env.get("e2e.redis.location.endpoint")}",
                "e2e.location.key.prefix=${Env.get("e2e.location.key.prefix")}",
                "e2e.api.endpoint=${options.filteredApiEndpoint}",
                "e2e.http.endpoint=${options.filteredServiceHttp}",
                "e2e.mesh.endpoint=${options.filteredMeshEndpoint}",
                "e2e.log.dir=${options.logDir}",
            ).joinToString("\n") + "\n",
        )
        return ProcessBuilder(options.filteredServiceBin, "--e2e-config", config.toString())
            .redirectOutput(Path.of(options.logDir, "filtered-service-replacement.stdout.log").toFile())
            .redirectError(Path.of(options.logDir, "filtered-service-replacement.stderr.log").toFile())
            .start()
    }

    fun waitForPort(open: Boolean, message: String) {
        waitForEndpoint(open, options.filteredServiceHttp, message)
    }

    fun waitForMesh(open: Boolean, message: String) {
        waitForEndpoint(open, options.filteredMeshEndpoint, message)
    }

    private fun waitForEndpoint(open: Boolean, endpoint: String, message: String) {
        val uri = URI.create(endpoint)
        repeat(200) {
            if (canConnect(uri) == open) return
            ScenarioAssert.sleep(100)
        }
        throw IllegalStateException(message)
    }

    fun stop(process: Process?) {
        if (process == null) return
        process.destroy()
        if (!process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            process.waitFor(5, TimeUnit.SECONDS)
        }
    }

    private fun canConnect(uri: URI): Boolean = try {
        Socket(uri.host, uri.port).use { true }
    } catch (_: Exception) {
        false
    }
}
