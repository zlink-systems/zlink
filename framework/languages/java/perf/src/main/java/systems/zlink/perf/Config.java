package systems.zlink.perf;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import java.util.List;

/** Application control/config JSON only; Framework owns all message serialization. */
public record Config(JsonNode root) {
    public Config {
        for (String key : List.of("connections", "logicalStreams")) {
            JsonNode value=root.path("workload").path(key);
            if (!value.isMissingNode() && !value.isNull() && (!value.isIntegralNumber() || !value.canConvertToInt() || value.asInt()<1 || value.asInt()>1000))
                throw new IllegalArgumentException("workload."+key+" must be between 1 and 1000; inflight is separate from CCU");
        }
    }
    public static final ObjectMapper JSON = new ObjectMapper();
    public static Config read(String path) throws Exception { return new Config(JSON.readTree(Path.of(path).toFile())); }
    public String text(String key) { return root.path(key).asText(""); }
    public boolean source() { return root.path("source").asBoolean(false); }
    public int instance() { return root.path("roleInstance").asInt(); }
    public String scenario() { return text("scenario"); }
    public String mode() { return text("mode"); }
    public int number(String key, int fallback) { return root.path("workload").path(key).asInt(fallback); }
    public double seconds(String key, double fallback) { return root.path("workload").path(key).asDouble(fallback); }
    public List<String> list(String key) {
        return java.util.stream.StreamSupport.stream(root.path(key).spliterator(), false).map(JsonNode::asText).toList();
    }
    public boolean cs() { return scenario().startsWith("cs-") || scenario().equals("session-echo-only"); }
    public boolean objects() { return !text("objectRole").equalsIgnoreCase("None") && !text("objectRole").isEmpty(); }
    public boolean objectServer() { return text("objectRole").toLowerCase().contains("server"); }
    public boolean sendSend() { return mode().equals("send-send") || scenario().contains("send-send"); }
    public boolean oneWay() { return mode().equals("send") || scenario().endsWith("-send"); }
    public boolean publish() { return scenario().equals("pubsub-fanout-echo"); }
    public boolean echo() { return !oneWay() && !publish(); }
    public boolean spotDriver() { return scenario().startsWith("s2s-spot-to-channel"); }
    public boolean worker() { return scenario().equals("spot-worker-offload-echo"); }
    public boolean yielding() { return text("terminal").equals("yield"); }
    public String returnChannel() { return text("channelName") + ".return"; }
    public int requestBytes() { return number(echo() && !sendSend() ? "requestPayloadBytes" : "sendPayloadBytes", echo() && !sendSend() ? 64 : 4096); }
    public int replyBytes() { return number("responsePayloadBytes",4096); }
    public int streams() { return number("logicalStreams",1); }
    public String listener() { return cs()&&!text("meshEndpoint").isEmpty()?text("meshEndpoint"):text("listenerEndpoint"); }
}
