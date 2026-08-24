import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import java.util.HexFormat;

public class WEquivActorJoin28 {
    public static void main(String[] args) throws Exception {
        var actor = new ServiceWirePilotCodec.Fence("actor", 2L, new byte[]{1,2}, 3L, 4L, 5L);
        var spot = new ServiceWirePilotCodec.Fence("spot", 6L, new byte[]{7,8}, 9L, 10L, 11L);
        var join = new ServiceWirePilotCodec.ActorJoin28(1L, actor, true, spot);
        byte[] bytes = ServiceWirePilotCodec.encodeActorJoin28(join);
        String hex = HexFormat.of().formatHex(bytes);
        String expected = "5a4d011c000000000000000001056163746f720000000000000002020102000000000000000300000000000000040000000000000005010473706f7400000000000000060207080000000000000009000000000000000a000000000000000b";
        System.out.println("generated: " + hex);
        System.out.println("expected : " + expected);
        System.out.println("equal: " + hex.equals(expected));
    }
}
