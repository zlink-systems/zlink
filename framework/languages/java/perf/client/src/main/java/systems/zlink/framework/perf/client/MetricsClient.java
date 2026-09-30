package systems.zlink.framework.perf.client;

import systems.zlink.framework.perf.DecimalText;
import systems.zlink.framework.perf.EndpointManifest;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfJson;
import systems.zlink.framework.perf.PerfTriggerReply;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.ResetReply;
import systems.zlink.framework.perf.ResetRequest;

import java.io.IOException;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// Only the standalone application client sends phase triggers. HTTP acknowledgements are not echo operations.
public final class MetricsClient implements AutoCloseable {
    private final EndpointManifest manifest;
    private final HttpClient http;

    public MetricsClient(EndpointManifest manifest) {
        this.manifest = manifest;
        this.http = HttpClient.newBuilder().connectTimeout(Duration.ofMillis(manifest.workload().adminTimeoutMs())).build();
    }

    private HttpResponse<String> post(String url, Object body) throws IOException, InterruptedException {
        HttpRequest request = HttpRequest.newBuilder(URI.create(url))
                .timeout(Duration.ofMillis(manifest.workload().adminTimeoutMs()))
                .header("Content-Type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(PerfJson.write(body), StandardCharsets.UTF_8))
                .build();
        return http.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
    }

    public List<Object> triggerRoles(PerfTriggerRequest request) throws IOException, InterruptedException {
        List<Object> acknowledgements = new ArrayList<>();
        // Manifest order is receivers then source, fixed by the coordinator before processes start.
        for (EndpointManifest.EndpointRole role : manifest.roles()) {
            long sent = PerfClock.now();
            HttpResponse<String> response = post(role.applicationTriggerUrl(), request);
            long received = PerfClock.now();
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("Trigger " + role.role() + "/" + role.roleInstance() + ": HTTP "
                        + response.statusCode() + ": " + response.body());
            }
            PerfTriggerReply ack = PerfJson.read(response.body(), PerfTriggerReply.class);
            if (!ack.accepted() || !ack.runId().equals(request.runId()) || !ack.cellId().equals(request.cellId())
                    || !ack.resetSeq().equals(request.resetSeq()) || !ack.phase().equals(request.phase())
                    || !ack.configHash().equals(manifest.configHash())) {
                throw new PerfValidationException("PhaseMismatch", "Role trigger acknowledgement identity differs.");
            }
            Map<String, Object> entry = new LinkedHashMap<>();
            entry.put("role", role.role());
            entry.put("roleInstance", role.roleInstance());
            entry.put("sentTicks", DecimalText.of(sent));
            entry.put("ackTicks", DecimalText.of(received));
            entry.put("clockDomainId", PerfClock.DOMAIN);
            entry.put("acknowledgement", ack);
            acknowledgements.add(entry);
        }
        return acknowledgements;
    }

    public List<ResetReply> resetRoles(ResetRequest request) throws IOException, InterruptedException {
        List<ResetReply> acknowledgements = new ArrayList<>();
        for (EndpointManifest.EndpointRole role : manifest.roles()) {
            HttpResponse<String> response = post(role.metrics().baseUrl() + "/perf/reset", request);
            if (response.statusCode() / 100 != 2) {
                throw new IllegalStateException("Reset " + role.role() + "/" + role.roleInstance() + ": HTTP "
                        + response.statusCode() + ": " + response.body());
            }
            ResetReply ack = PerfJson.read(response.body(), ResetReply.class);
            if (!ack.ok() || !ack.runId().equals(request.runId()) || !ack.cellId().equals(request.cellId())
                    || !ack.resetSeq().equals(request.resetSeq())) {
                throw new PerfValidationException("PhaseMismatch", "Role reset acknowledgement identity differs.");
            }
            acknowledgements.add(ack);
        }
        return acknowledgements;
    }

    @Override
    public void close() {
        http.close();
    }
}
