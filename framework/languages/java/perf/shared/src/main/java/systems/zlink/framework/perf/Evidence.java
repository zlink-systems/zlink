package systems.zlink.framework.perf;

import java.util.LinkedHashMap;
import java.util.Map;

// §16.1 evidence entry: kind, source and the observed value (public status, typed reply or marker).
public final class Evidence {
    private Evidence() {}

    public static Map<String, Object> of(String kind, String source, Object observedValue) {
        Map<String, Object> evidence = new LinkedHashMap<>();
        evidence.put("kind", kind);
        evidence.put("source", source);
        evidence.put("observedValue", observedValue);
        return evidence;
    }
}
