package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.internal.sockets.SocketOptions;
import systems.zlink.internal.sockets.SocketOptionValueType;
import systems.zlink.internal.sockets.SocketOption;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.CommonSocketOptions;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.PubSocketOptions;
import systems.zlink.contracts.sockets.XPubSocket;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

public class SocketOptionsTypeMapTest {
    private static final Set<Integer> NON_SOCKET_CATALOG_OPTION_IDS = Set.of(
      SocketOption.SUBSCRIBE.getValue(),
      SocketOption.UNSUBSCRIBE.getValue(),
      SocketOption.RCVMORE.getValue(),
      SocketOption.AUTO_HWM_MSG_UNIT_BYTES.getValue()
    );

    @Test
    public void catalogCoversSocketOptionEnum() {
        List<SocketOptionKey<?>> keys = SocketOptions.all();
        assertFalse(keys.isEmpty());

        Map<Integer, Integer> idCounts = new HashMap<>();
        Map<Integer, Set<String>> idToNames = new HashMap<>();
        Set<String> names = new HashSet<>();
        for (SocketOptionKey<?> key : keys) {
            assertTrue(names.add(key.name()), "duplicate key name: " + key.name());
            idCounts.merge(key.optionId(), 1, Integer::sum);
            idToNames.computeIfAbsent(key.optionId(), ignored -> new HashSet<>())
                .add(key.name());
            assertTrue(key.readable() || key.writable(),
                "invalid key access mode: " + key.name());
        }

        for (SocketOption option : SocketOption.values()) {
            if (NON_SOCKET_CATALOG_OPTION_IDS.contains(option.getValue()))
                continue;
            assertTrue(idCounts.containsKey(option.getValue()),
                "missing mapping for enum option: " + option.name());
        }

        int aliasId = SocketOption.TLS_VERIFY.getValue();
        for (Map.Entry<Integer, Set<String>> entry : idToNames.entrySet()) {
            Set<String> mapped = entry.getValue();
            if (mapped.size() <= 1)
                continue;
            if (entry.getKey() == aliasId) {
                assertEquals(Set.of("TLS_VERIFY", "XPUB_MANUAL_LAST_VALUE"),
                    mapped, "unexpected alias mapping for option id 98");
                continue;
            }
            if (isStringBytesTwin(mapped))
                continue;
            fail("unexpected duplicate option id " + entry.getKey()
                + ": " + mapped);
        }
    }

    @Test
    public void stringFirstAndBytesTwinKeysAreConfigured() {
        assertEquals(String.class, SocketOptions.ROUTING_ID.valueClass());
        assertEquals(byte[].class, SocketOptions.ROUTING_ID_BYTES.valueClass());
        assertEquals(SocketOptions.ROUTING_ID.optionId(),
            SocketOptions.ROUTING_ID_BYTES.optionId());

        assertEquals(String.class, SocketOptions.CONNECT_ROUTING_ID.valueClass());
        assertEquals(byte[].class,
            SocketOptions.CONNECT_ROUTING_ID_BYTES.valueClass());
        assertEquals(SocketOptions.CONNECT_ROUTING_ID.optionId(),
            SocketOptions.CONNECT_ROUTING_ID_BYTES.optionId());
    }

    @Test
    public void highWaterMarkKeysUseUnsigned64BitValues() {
        assertEquals(Long.class, SocketOptions.SNDHWM.valueClass());
        assertEquals(Long.class, SocketOptions.RCVHWM.valueClass());
        assertEquals(SocketOptionValueType.UINT64,
            SocketOptions.SNDHWM.valueType());
        assertEquals(SocketOptionValueType.UINT64,
            SocketOptions.RCVHWM.valueType());
    }

    @Test
    public void byteHighWaterMarksRoundTrip64BitBoundaries() {
        TestSupport.assumeNative();
        try (Context ctx = Zlink.createContext()) {
            ctx.options().autoHwmEnabled(false);
            try (PairSocket pair = ctx.createPairSocket()) {
                assertEquals(4_096_000L, pair.options().sendHwm());
                assertEquals(4_096_000L, pair.options().recvHwm());

                long beyondInt32 = (long) Integer.MAX_VALUE + 65_536L;
                pair.options().sendHwm(beyondInt32);
                pair.options().recvHwm(Long.MAX_VALUE);
                assertEquals(beyondInt32, pair.options().sendHwm());
                assertEquals(Long.MAX_VALUE, pair.options().recvHwm());

                pair.options().sendHwm(0L);
                pair.options().recvHwm(0L);
                assertEquals(0L, pair.options().sendHwm());
                assertEquals(0L, pair.options().recvHwm());
                pair.options().sendHwm(-1L);
                assertEquals(-1L, pair.options().sendHwm());
            }
        }
    }

    @Test
    public void aliasOptionId98RespectsSocketTypeOwner() {
        try (Context ctx = Zlink.createContext();
             PairSocket pair = ctx.createPairSocket();
             XPubSocket xpub = ctx.createXPubSocket()) {
            assertTrue(pair.options() instanceof CommonSocketOptions);
            assertTrue(xpub.options() instanceof PubSocketOptions);
            xpub.options().manualLastValue(true);
            assertTrue(xpub.options().manualLastValue());
        }
    }

    private static boolean isStringBytesTwin(Set<String> mapped) {
        if (mapped.size() != 2)
            return false;
        String[] names = mapped.toArray(String[]::new);
        return isBytesTwin(names[0], names[1]) || isBytesTwin(names[1], names[0]);
    }

    private static boolean isBytesTwin(String maybeBase, String maybeBytes) {
        if (!maybeBytes.endsWith("_BYTES"))
            return false;
        String base = maybeBytes.substring(0, maybeBytes.length() - "_BYTES".length());
        return base.equals(maybeBase);
    }
}
