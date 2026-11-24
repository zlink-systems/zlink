package systems.zlink.framework.runtime.internal.json;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.JsonMappingException;
import com.fasterxml.jackson.databind.exc.InvalidNullException;

import java.io.Closeable;
import java.io.IOException;

/** A typed JSON failure whose message contains wire names and never payload values. */
public final class ZLinkJsonDiagnostic extends IllegalArgumentException {
    private enum Kind {
        NULL,
        DECIMAL_STRING
    }

    private static final class Violation extends Exception {
        private final Kind kind;
        private final String property;

        private Violation(Kind kind, String property) {
            super(null, null, false, false);
            this.kind = kind;
            this.property = property;
        }
    }

    private ZLinkJsonDiagnostic(String message, IOException cause) {
        super(message, cause);
    }

    static JsonMappingException nullProperty(Closeable source, String property) {
        return new JsonMappingException(
                source,
                "Property '" + property + "' does not allow null.",
                new Violation(Kind.NULL, property));
    }

    static JsonMappingException decimalStringExpected(JsonParser parser) {
        return JsonMappingException.from(
                parser,
                "64-bit integers must be decimal JSON strings",
                new Violation(Kind.DECIMAL_STRING, null));
    }

    public static ZLinkJsonDiagnostic from(Class<?> messageType, IOException failure) {
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if (cause instanceof InvalidNullException invalidNull) {
                var name = invalidNull.getPropertyName();
                return new ZLinkJsonDiagnostic(
                        describe(
                                messageType,
                                invalidNull,
                                Kind.NULL,
                                name == null ? null : name.getSimpleName()),
                        failure);
            }
            if (cause instanceof JsonMappingException mapping
                    && mapping.getCause() instanceof Violation violation) {
                return new ZLinkJsonDiagnostic(
                        describe(messageType, mapping, violation.kind, violation.property),
                        failure);
            }
        }
        return null;
    }

    private static String describe(
            Class<?> messageType, JsonMappingException mapping, Kind kind, String property) {
        var path = new StringBuilder();
        String lastField = null;
        for (var reference : mapping.getPath()) {
            lastField = reference.getFieldName();
            if (lastField != null) {
                appendField(path, lastField);
            } else if (reference.getIndex() >= 0) {
                path.append('[').append(reference.getIndex()).append(']');
            }
        }
        if (property != null && !property.equals(lastField)) {
            appendField(path, property);
        }
        String advice =
                switch (kind) {
                    case NULL ->
                            "does not allow null. Declare it nullable (@Nullable). "
                                    + "@ZLinkJsonOptional only allows omission.";
                    case DECIMAL_STRING ->
                            "expects a decimal string for a 64-bit integer (framework-json-v1 §5).";
                };
        return "Message '"
                + messageType.getName()
                + "': Property '"
                + (path.isEmpty() ? "$" : path)
                + "' "
                + advice;
    }

    private static void appendField(StringBuilder path, String field) {
        if (!path.isEmpty()) path.append('.');
        path.append(field);
    }
}
