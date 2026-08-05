package systems.zlink.e2e.toactormessaging.session;
import org.springframework.boot.context.properties.ConfigurationProperties;
@ConfigurationProperties("e2e")
public record SessionOptions(String sessionRid, String sessionHttpEndpoint, String sessionSpotEndpoint,
    String sessionStreamEndpoint, String redisLocationEndpoint, String locationKeyPrefix) {
    public SessionOptions { required(sessionRid,"session-rid"); required(sessionHttpEndpoint,"session-http-endpoint");
        required(sessionSpotEndpoint,"session-spot-endpoint"); required(sessionStreamEndpoint,"session-stream-endpoint");
        required(redisLocationEndpoint,"redis-location-endpoint"); required(locationKeyPrefix,"location-key-prefix"); }
    private static void required(String v,String n){if(v==null||v.isBlank())throw new IllegalArgumentException("e2e."+n+" is required");}
}
