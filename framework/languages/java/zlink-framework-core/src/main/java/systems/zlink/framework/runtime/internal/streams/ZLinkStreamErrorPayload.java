package systems.zlink.framework.runtime.internal.streams;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.util.Map;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Encodes and decodes the common JSON body carried by a STREAM Error frame. */
public final class ZLinkStreamErrorPayload {
    private static final ObjectMapper MAPPER = new ObjectMapper();

    private ZLinkStreamErrorPayload() {
    }

    public static byte[] encode(Throwable failure) {
        Throwable error = unwrap(failure);
        String code = error instanceof ZLinkFrameworkException frameworkError
            ? frameworkCode(frameworkError.kind())
            : error.getClass().getSimpleName();
        String message = error.getMessage();
        if (message == null || message.isBlank()) {
            message = error.getClass().getSimpleName();
        }
        try {
            return MAPPER.writeValueAsBytes(Map.of(
                "code", code,
                "message", message));
        } catch (JsonProcessingException encodingFailure) {
            throw new IllegalStateException(
                "failed to encode STREAM error payload", encodingFailure);
        }
    }

    public static Decoded decode(byte[] payload) {
        if (payload == null || payload.length == 0) {
            throw new IllegalArgumentException("STREAM error payload is empty");
        }
        try {
            JsonNode root = MAPPER.readTree(payload);
            if (root == null || !root.isObject()) {
                throw new IllegalArgumentException("STREAM error payload must be a JSON object");
            }
            JsonNode codeNode = root.get("code");
            JsonNode messageNode = root.get("message");
            if (codeNode == null || !codeNode.isTextual()
                || messageNode == null || !messageNode.isTextual()
                || codeNode.textValue().isBlank() || messageNode.textValue().isBlank()) {
                throw new IllegalArgumentException(
                    "STREAM error payload requires non-empty code and message strings");
            }
            String code = codeNode.textValue();
            ZLinkFrameworkErrorKind frameworkKind = frameworkKind(code);
            return new Decoded(code, messageNode.textValue(), frameworkKind);
        } catch (IOException malformed) {
            throw new IllegalArgumentException("STREAM error payload is not valid JSON", malformed);
        }
    }

    public record Decoded(
        String code,
        String message,
        ZLinkFrameworkErrorKind frameworkKind) {
    }

    private static ZLinkFrameworkErrorKind frameworkKind(String code) {
        for (ZLinkFrameworkErrorKind kind : ZLinkFrameworkErrorKind.values()) {
            if (frameworkCode(kind).equals(code)) {
                return kind;
            }
        }
        return null;
    }

    private static String frameworkCode(ZLinkFrameworkErrorKind kind) {
        StringBuilder result = new StringBuilder();
        boolean uppercase = true;
        for (char value : kind.name().toCharArray()) {
            if (value == '_') {
                uppercase = true;
                continue;
            }
            result.append(uppercase ? value : Character.toLowerCase(value));
            uppercase = false;
        }
        return result.toString();
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure == null
            ? new IllegalStateException("handler failed")
            : failure;
        while ((current instanceof java.util.concurrent.CompletionException
                || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
