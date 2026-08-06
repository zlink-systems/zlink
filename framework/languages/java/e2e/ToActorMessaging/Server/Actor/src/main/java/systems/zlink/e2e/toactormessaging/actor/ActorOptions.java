package systems.zlink.e2e.toactormessaging.actor;
import org.springframework.boot.context.properties.ConfigurationProperties;
@ConfigurationProperties("e2e")
public record ActorOptions(String actorHttpEndpoint, String actorSpotEndpoint, String actorRid,
    String redisLocationEndpoint, String locationKeyPrefix, String logDirectory,
    String baselineActorIds, String actorAdvertiseHost) {
    public ActorOptions { required(actorHttpEndpoint,"actor-http-endpoint"); required(actorSpotEndpoint,"actor-spot-endpoint");
        required(actorRid,"actor-rid"); required(redisLocationEndpoint,"redis-location-endpoint");
        required(locationKeyPrefix,"location-key-prefix"); required(logDirectory,"log-directory");
        baselineActorIds = optional(baselineActorIds);
        actorAdvertiseHost = optional(actorAdvertiseHost); }
    private static void required(String v,String n){if(v==null||v.isBlank())throw new IllegalArgumentException("e2e."+n+" is required");}
    private static String optional(String value) { return value == null ? "" : value.trim(); }
}
