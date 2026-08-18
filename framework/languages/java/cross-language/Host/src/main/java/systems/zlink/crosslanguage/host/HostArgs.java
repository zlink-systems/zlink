package systems.zlink.crosslanguage.host;

import java.util.HashMap;
import java.util.Map;

/**
 * Minimal CLI arg holder mirroring the C++/.NET/Node cross-language peer
 * hosts: a positional mode followed by {@code --key value} pairs. Avoids
 * Spring's ConfigurationProperties/YAML machinery so a single process
 * invocation stays a one-liner, matching the other three language peers.
 */
public final class HostArgs {
    private final String mode;
    private final Map<String, String> options = new HashMap<>();

    public HostArgs(String[] args) {
        String parsedMode = null;
        int index = 0;
        if (args.length > 0 && !args[0].startsWith("--")) {
            parsedMode = args[0];
            index = 1;
        }
        for (; index < args.length; index++) {
            String argument = args[index];
            if (argument.startsWith("--") && index + 1 < args.length) {
                options.put(argument.substring(2), args[index + 1]);
                index++;
            }
        }
        this.mode = parsedMode == null ? "idle" : parsedMode;
    }

    public String mode() {
        return mode;
    }

    public String option(String name, String fallback) {
        return options.getOrDefault(name, fallback);
    }

    public String require(String name) {
        String value = options.get(name);
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException("--" + name + " is required");
        }
        return value;
    }
}
