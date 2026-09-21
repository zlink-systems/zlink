package systems.zlink.framework.runtime.internal.locations;

public record ZLinkCreationTerminalFound(byte[] terminalEnvelope)
        implements ZLinkCreationTerminalReadResult {
    public ZLinkCreationTerminalFound {
        terminalEnvelope = terminalEnvelope.clone();
    }

    @Override
    public byte[] terminalEnvelope() {
        return terminalEnvelope.clone();
    }
}
