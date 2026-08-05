package systems.zlink.e2e.toactormessaging.client;
import java.io.Reader; import java.nio.file.Files; import java.nio.file.Path; import java.util.Properties;
public record ClientOptions(String actorHttpEndpoint,String callerHttpEndpoint,String sessionAHttpEndpoint,
    String sessionBHttpEndpoint,String sessionAStreamEndpoint,String sessionBStreamEndpoint) {
    public ClientOptions { required(actorHttpEndpoint,"actorHttpEndpoint"); required(callerHttpEndpoint,"callerHttpEndpoint");
        required(sessionAHttpEndpoint,"sessionAHttpEndpoint"); required(sessionBHttpEndpoint,"sessionBHttpEndpoint");
        required(sessionAStreamEndpoint,"sessionAStreamEndpoint"); required(sessionBStreamEndpoint,"sessionBStreamEndpoint"); }
    public static ClientOptions load(String path){Properties p=new Properties();try(Reader r=Files.newBufferedReader(Path.of(path))){p.load(r);}
        catch(Exception e){throw new IllegalStateException("Could not load ToActorMessaging client config",e);}
        return new ClientOptions(req(p,"actorHttpEndpoint"),req(p,"callerHttpEndpoint"),req(p,"sessionAHttpEndpoint"),
            req(p,"sessionBHttpEndpoint"),req(p,"sessionAStreamEndpoint"),req(p,"sessionBStreamEndpoint"));}
    private static String req(Properties p,String n){String v=p.getProperty(n);required(v,n);return v;}
    private static void required(String v,String n){if(v==null||v.isBlank())throw new IllegalArgumentException(n+" is required");}
}
