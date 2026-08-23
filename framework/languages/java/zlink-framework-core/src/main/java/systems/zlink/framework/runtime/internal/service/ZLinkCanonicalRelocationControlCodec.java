package systems.zlink.framework.runtime.internal.service;

import java.io.IOException;
import java.util.Objects;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

/** Strict canonical reader/preserver for service-wire relocation controls. */
final class ZLinkCanonicalRelocationControlCodec {
    public record Control(int command, byte[] encoded) {
        public Control {
            encoded = Objects.requireNonNull(encoded, "encoded").clone();
        }

        @Override
        public byte[] encoded() {
            return encoded.clone();
        }
    }

    public Control decode(byte[] encoded) {
        byte[] bytes = Objects.requireNonNull(encoded, "encoded").clone();
        if (bytes.length < 4) {
            throw invalid("prefix");
        }
        int command = Byte.toUnsignedInt(bytes[3]);
        try {
            return new Control(command, generatedRoundTrip(command, bytes));
        } catch (IOException failure) {
            throw invalid("command", failure);
        }
    }

    public byte[] encode(Control control) {
        Objects.requireNonNull(control, "control");
        Control decoded = decode(control.encoded());
        if (decoded.command() != control.command()) {
            throw invalid("command summary");
        }
        return decoded.encoded();
    }

    private static byte[] generatedRoundTrip(int command, byte[] bytes)
        throws IOException {
        return switch (command) {
            case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                ServiceWirePilotCodec.encodeRelocationReady30(
                    ServiceWirePilotCodec.decodeRelocationReady30(bytes));
            case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                ServiceWirePilotCodec.encodeRelocationData31(
                    ServiceWirePilotCodec.decodeRelocationData31(bytes));
            case ServiceWireConstants.COMMAND_RELOCATION_CUTOVER ->
                ServiceWirePilotCodec.encodeRelocationCutover34(
                    ServiceWirePilotCodec.decodeRelocationCutover34(bytes));
            case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                ServiceWirePilotCodec.encodeRelocationPrepare40(
                    ServiceWirePilotCodec.decodeRelocationPrepare40(bytes));
            case ServiceWireConstants.COMMAND_RELOCATION_STATE ->
                ServiceWirePilotCodec.encodeRelocationState52(
                    ServiceWirePilotCodec.decodeRelocationState52(bytes));
            case ServiceWireConstants.COMMAND_RELOCATION_FAILED ->
                ServiceWirePilotCodec.encodeRelocationFailed53(
                    ServiceWirePilotCodec.decodeRelocationFailed53(bytes));
            default -> throw invalid("command");
        };
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException(
            "invalid canonical relocation control " + field);
    }

    private static IllegalArgumentException invalid(
        String field, Throwable cause) {
        return new IllegalArgumentException(
            "invalid canonical relocation control " + field, cause);
    }
}
