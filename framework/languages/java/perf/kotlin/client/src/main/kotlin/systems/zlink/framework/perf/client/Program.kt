package systems.zlink.framework.perf.client

import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import systems.zlink.framework.perf.EndpointManifest
import systems.zlink.framework.perf.PerfJson

fun main(args: Array<String>) {
    require(args.size == 4 && args[0] == "--endpoint-config" && args[2] == "--client-index") {
        "Client requires --endpoint-config <file> --client-index <index>."
    }
    val index = args[3].toInt()
    val manifest = PerfJson.read(Files.readString(Path.of(args[1]), StandardCharsets.UTF_8), EndpointManifest::class.java)
    require(index >= 0 && index < manifest.workload().clientCount()) { "client index" }
    ClientControl.run(manifest, index) { scenarioManifest, measurement, clientIndex, _ ->
        SessionEchoScenario(scenarioManifest, measurement, clientIndex)
    }
}
