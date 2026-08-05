package systems.zlink.e2e.toactormessaging.caller;
import org.springframework.boot.context.properties.ConfigurationProperties;
@ConfigurationProperties("e2e")
public record CallerOptions(String callerHttpEndpoint, String callerSpotEndpoint, String callerRid,
    String redisLocationEndpoint, String locationKeyPrefix, String logDirectory) {
    public CallerOptions { required(callerHttpEndpoint,"caller-http-endpoint"); required(callerSpotEndpoint,"caller-spot-endpoint");
        required(callerRid,"caller-rid"); required(redisLocationEndpoint,"redis-location-endpoint");
        required(locationKeyPrefix,"location-key-prefix"); required(logDirectory,"log-directory"); }
    private static void required(String v,String n){if(v==null||v.isBlank())throw new IllegalArgumentException("e2e."+n+" is required");}
}
