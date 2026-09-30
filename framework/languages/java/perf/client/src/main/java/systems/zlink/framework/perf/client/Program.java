package systems.zlink.framework.perf.client;

import systems.zlink.framework.perf.EndpointManifest;
import systems.zlink.framework.perf.PerfJson;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

// The standalone application client (§6.1): option parsing and scenario selection only. CS cells run the connector
// loop here; every other cell's client only forwards the runner's phase triggers (§8.5). The runner drives it through the
// stdin/stdout JSON control pipe of §16.
public final class Program {
    private Program() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 4 || !"--endpoint-config".equals(args[0]) || !"--client-index".equals(args[2])) {
            throw new IllegalArgumentException("Client requires --endpoint-config <file> --client-index <index>.");
        }
        int index = Integer.parseInt(args[3]);
        EndpointManifest manifest = PerfJson.read(Files.readString(Path.of(args[1]), StandardCharsets.UTF_8), EndpointManifest.class);
        if (index < 0 || index >= manifest.workload().clientCount()) {
            throw new IndexOutOfBoundsException("client index");
        }
        ClientControl.run(manifest, index, (scenarioManifest, measurement, clientIndex, scenarioName) -> switch (scenarioName) {
            // Every CS cell shares the connector loop of the baseline; the cell id names which standard scenario runs.
            case "cs-local-session-actor-echo" -> new CsLocalSessionActorEchoScenario(scenarioManifest, measurement, clientIndex);
            case "cs-remote-session-actor-echo" -> new CsRemoteSessionActorEchoScenario(scenarioManifest, measurement, clientIndex);
            default -> new SessionEchoOnlyScenario(scenarioManifest, measurement, clientIndex);
        });
    }
}
