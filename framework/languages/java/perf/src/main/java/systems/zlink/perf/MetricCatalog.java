package systems.zlink.perf;

import com.fasterxml.jackson.databind.JsonNode;
import java.io.IOException;
import java.util.Map;

/** Shared result keys have one owner in framework/perf-contract. */
public final class MetricCatalog {
    private static final JsonNode CATALOG;
    static {
        try (var input=MetricCatalog.class.getResourceAsStream("/metric-catalog.json")) {
            if(input==null)throw new IllegalStateException("Shared metric catalog is missing");
            CATALOG=Config.JSON.readTree(input);
        }catch(IOException error){throw new ExceptionInInitializerError(error);}
    }
    private MetricCatalog(){}
    public static void complete(Map<String,Object> metrics,Map<String,Object> histograms,Map<String,Object> reasons){
        for(String group:new String[]{"counts","rates","unsupported"})for(JsonNode key:CATALOG.path(group))
            missing(metrics,reasons,"metrics",key.asText(),group.equals("unsupported")?"PUBLIC_OBSERVATION_UNSUPPORTED":"NOT_APPLICABLE");
        for(JsonNode prefix:CATALOG.path("latencyPrefixes"))for(JsonNode suffix:CATALOG.path("latencySuffixes"))
            missing(metrics,reasons,"metrics",prefix.asText()+"."+suffix.asText(),"NOT_APPLICABLE");
        CATALOG.path("histogramPrefixes").fieldNames().forEachRemaining(key->missing(histograms,reasons,"histograms",key,"NOT_APPLICABLE"));
        for(JsonNode key:CATALOG.path("errorNamespaces"))metrics.putIfAbsent(key.asText(),Map.of());
    }
    private static void missing(Map<String,Object> values,Map<String,Object> reasons,String container,String key,String code){
        if(values.containsKey(key))return;
        values.put(key,null);reasons.put("/"+container+"/"+key,Measurement.reason(code,"This role has no applicable public observation"));
    }
}
