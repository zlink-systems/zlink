package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;

/** Fresh JVM probe: compatibility must be checked before any snapshot call. */
public final class ConnectionContextProbe {
    public static void main(String[] args) {
        try (Context context = Zlink.createContext()) {
            System.out.println("CONTEXT_CREATED");
        }
    }
}
