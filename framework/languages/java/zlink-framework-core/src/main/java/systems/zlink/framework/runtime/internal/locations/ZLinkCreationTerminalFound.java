package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkCreationTerminalFound(
    ZLinkCreationOperationTerminal terminal)
    implements ZLinkCreationTerminalReadResult {
    public ZLinkCreationTerminalFound {
        Objects.requireNonNull(terminal, "terminal");
    }
}
