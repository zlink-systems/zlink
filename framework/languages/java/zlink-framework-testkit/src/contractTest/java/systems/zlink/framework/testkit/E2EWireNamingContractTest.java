package systems.zlink.framework.testkit;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;
import org.junit.jupiter.api.Test;

/** Guards the operation-first names used by the Java and Kotlin E2E wire contracts. */
final class E2EWireNamingContractTest {
    private static final Pattern WIRE_SUFFIX =
        Pattern.compile("[A-Za-z][A-Za-z0-9_]*(?:Req|Res|Msg|Notify|Event)");
    private static final Pattern FORBIDDEN_WIRE_ALIAS = Pattern.compile(
        "[A-Za-z][A-Za-z0-9_]*(?:Command|Result|Ack|Request|Reply|Response|Notification)");
    private static final Pattern CONTRACT_DECLARATION = Pattern.compile(
        "\\b(?:record|class|interface|message)\\s+([A-Z][A-Za-z0-9_]*)");
    private static final Pattern HANDLER_CONTRACT = Pattern.compile(
        "\\b(ZLink[A-Za-z0-9_]*(?:Request|Send|Packet|Subscription|Publish)Handler)"
            + "\\s*<([^>]+)>",
        Pattern.DOTALL);
    private static final Pattern LITERAL_PACKET_NAME = Pattern.compile(
        "(?:packetName\\s*=\\s*|packetName\\s*\\(\\s*\\)"
            + "\\s*(?::\\s*String)?\\s*=\\s*"
            + "|@ZLinkPacket\\s*\\(\\s*"
            + "|\\braw\\s*\\(\\s*"
            + "|(?:new\\s+)?ZLink(?:Stream)?EncodedPayload\\s*\\(\\s*)"
            + "\"([A-Za-z][A-Za-z0-9_]*)\"");
    private static final Pattern JAVA_PACKET_NAME_METHOD = Pattern.compile(
        "packetName\\s*\\([^)]*\\)[^{]{0,100}\\{\\s*return\\s+"
            + "\"([A-Za-z][A-Za-z0-9_]*)\"",
        Pattern.DOTALL);
    private static final Pattern HANDLER_PACKET_ANNOTATION = Pattern.compile(
        "@ZLink(?:Spot|EntrySpot|Actor|Route|Channel)[A-Za-z0-9_]*"
            + "\\s*\\([^)]{0,300}?packetName\\s*=\\s*"
            + "\"([A-Za-z][A-Za-z0-9_]*)\"",
        Pattern.DOTALL);
    private static final Pattern WIRE_CALL = Pattern.compile(
        "\\.(requestToActor|requestToSpot|requestToNode|requestToChannel|request"
            + "|sendToActor|sendToSpot|sendToNode|sendToChannel|send"
            + "|publishToSpot|publishToFanout|publish)\\s*\\(");
    private static final Pattern CREATE_CALL = Pattern.compile(
        "\\.getOrCreate\\s*\\("
            + "|\\b(?:actors|spots|actorManager|spotManager)(?:\\(\\))?"
            + "\\.create\\s*\\(");
    private static final Pattern TYPE_TOKEN = Pattern.compile(
        "(?:[A-Za-z_][A-Za-z0-9_]*\\.)*([A-Z][A-Za-z0-9_]*)");

    @Test
    void wireDeclarationsAndLiteralPacketNamesUseOnlyOperationSuffixes() throws IOException {
        Map<Path, List<String>> offenders = new LinkedHashMap<>();
        for (SourceFile source : sources()) {
            List<String> violations = new ArrayList<>();
            if (isContractDeclarationSource(source.path())) {
                Matcher declarations = CONTRACT_DECLARATION.matcher(source.content());
                while (declarations.find()) {
                    String name = declarations.group(1);
                    if (FORBIDDEN_WIRE_ALIAS.matcher(name).matches()) {
                        violations.add(at(source, declarations.start(1),
                            "wire declaration retains forbidden alias " + name));
                    }
                }
            }
            collectPacketNameViolations(source, LITERAL_PACKET_NAME, violations);
            collectPacketNameViolations(source, JAVA_PACKET_NAME_METHOD, violations);
            if (!violations.isEmpty()) {
                offenders.put(source.path(), violations);
            }
        }
        assertTrue(offenders.isEmpty(),
            "E2E wire declarations and packet-name strings must use Req/Res/Msg/Notify/Event: "
                + offenders);
    }

    @Test
    void handlerContractsMatchTheirDispatchOperation() throws IOException {
        Map<Path, List<String>> offenders = new LinkedHashMap<>();
        for (SourceFile source : sources()) {
            List<String> violations = new ArrayList<>();
            List<String> handlerWireTypes = new ArrayList<>();
            Matcher handlers = HANDLER_CONTRACT.matcher(source.content());
            while (handlers.find()) {
                String handler = handlers.group(1);
                List<String> arguments = Arrays.stream(handlers.group(2).split(","))
                    .map(E2EWireNamingContractTest::simpleType)
                    .filter(name -> !name.isBlank())
                    .toList();
                if (handler.endsWith("RequestHandler") && arguments.size() >= 2) {
                    String request = arguments.get(arguments.size() - 2);
                    String response = arguments.get(arguments.size() - 1);
                    handlerWireTypes.add(request);
                    handlerWireTypes.add(response);
                    requireSuffix(source, handlers.start(), request, "Req", handler, violations);
                    requireSuffix(source, handlers.start(), response, "Res", handler, violations);
                } else if (handler.endsWith("SendHandler") && !arguments.isEmpty()) {
                    String message = arguments.get(arguments.size() - 1);
                    handlerWireTypes.add(message);
                    requireSuffix(source, handlers.start(), message, "Msg", handler, violations);
                } else if ((handler.endsWith("SubscriptionHandler")
                        || handler.endsWith("PublishHandler")) && !arguments.isEmpty()) {
                    String event = arguments.get(arguments.size() - 1);
                    handlerWireTypes.add(event);
                    requireSuffix(source, handlers.start(), event, "Event", handler, violations);
                } else if (handler.contains("SpotPacketHandler") && !arguments.isEmpty()) {
                    String message = arguments.get(arguments.size() - 1);
                    handlerWireTypes.add(message);
                    requireSuffix(source, handlers.start(), message, "Msg", handler, violations);
                }
            }

            Matcher annotations = HANDLER_PACKET_ANNOTATION.matcher(source.content());
            while (annotations.find()) {
                String packetName = annotations.group(1);
                Matcher typedReferences = Pattern.compile("\\b" + Pattern.quote(packetName) + "\\b")
                    .matcher(source.content());
                int references = 0;
                while (typedReferences.find()) {
                    references++;
                }
                if (!handlerWireTypes.contains(packetName) && references < 2) {
                    violations.add(at(source, annotations.start(1),
                        "handler packet name " + packetName
                            + " does not match an annotated method parameter or handler generic type"));
                }
            }
            if (!violations.isEmpty()) {
                offenders.put(source.path(), violations);
            }
        }
        assertTrue(offenders.isEmpty(),
            "request handlers require Req/Res, send handlers require Msg, "
                + "and publish handlers require Event: " + offenders);
    }

    @Test
    void resolvedWireCallSitesMatchTheirOperation() throws IOException {
        Map<Path, List<String>> offenders = new LinkedHashMap<>();
        for (SourceFile source : sources()) {
            List<String> violations = new ArrayList<>();
            Matcher calls = WIRE_CALL.matcher(source.content());
            while (calls.find()) {
                int open = calls.end() - 1;
                int close = matchingParen(source.content(), open);
                if (close < 0) {
                    continue;
                }
                List<String> arguments = topLevelArguments(source.content().substring(open + 1, close));
                if (arguments.isEmpty()) {
                    continue;
                }
                String type = expressionType(
                    arguments.get(arguments.size() - 1), source.content(), calls.start());
                if (type == null || type.equals("ZLinkMessage") || type.equals("Object")) {
                    continue;
                }
                if (type.matches("T[A-Z][A-Za-z0-9_]*")) {
                    continue;
                }
                if (!WIRE_SUFFIX.matcher(type).matches()
                        && !FORBIDDEN_WIRE_ALIAS.matcher(type).matches()) {
                    continue;
                }
                String operation = calls.group(1);
                String expected = operation.startsWith("request") ? "Req"
                    : operation.startsWith("publish") ? "Event"
                    : operation.equals("send") ? null : "Msg";
                if (expected == null) {
                    if (!type.endsWith("Msg") && !type.endsWith("Notify")) {
                        violations.add(at(source, calls.start(),
                            operation + " payload " + type + " must end in Msg or Notify"));
                    }
                } else if (!type.endsWith(expected)) {
                    violations.add(at(source, calls.start(),
                        operation + " payload " + type + " must end in " + expected));
                }
            }
            if (!violations.isEmpty()) {
                offenders.put(source.path(), violations);
            }
        }
        assertTrue(offenders.isEmpty(),
            "resolved request/send/publish call sites must use their operation suffix: " + offenders);
    }

    @Test
    void actorAndSpotCreateCallsAlwaysCarryTypedReqPayloads() throws IOException {
        Map<Path, List<String>> offenders = new LinkedHashMap<>();
        for (SourceFile source : sources()) {
            List<String> violations = new ArrayList<>();
            Matcher creates = CREATE_CALL.matcher(source.content());
            while (creates.find()) {
                int chainEnd = createChainEnd(source.content(), creates.end());
                String chain = source.content().substring(creates.end(), chainEnd);
                Matcher requestCall = Pattern.compile("\\.request\\s*\\(").matcher(chain);
                if (!requestCall.find()) {
                    violations.add(at(source, creates.start(),
                        "actor/Spot create call has no request payload"));
                    continue;
                }
                int open = creates.end() + requestCall.end() - 1;
                int close = matchingParen(source.content(), open);
                if (close < 0) {
                    violations.add(at(source, creates.start(), "could not parse create request payload"));
                    continue;
                }
                String expression = source.content().substring(open + 1, close);
                String type = expressionType(expression, source.content(), creates.start());
                if (type == null || !type.endsWith("Req")) {
                    violations.add(at(source, creates.start(),
                        "create payload must resolve to *Req but was "
                            + (type == null ? expression.strip().replaceAll("\\s+", " ") : type)));
                }
            }
            if (!violations.isEmpty()) {
                offenders.put(source.path(), violations);
            }
        }
        assertTrue(offenders.isEmpty(),
            "Actor/Spot create and getOrCreate calls must carry a typed *Req payload: " + offenders);
    }

    private static void collectPacketNameViolations(
        SourceFile source,
        Pattern pattern,
        List<String> violations) {
        Matcher matcher = pattern.matcher(source.content());
        while (matcher.find()) {
            String name = matcher.group(1);
            if (!WIRE_SUFFIX.matcher(name).matches()) {
                violations.add(at(source, matcher.start(1),
                    "packet name lacks a wire operation suffix: " + name));
            } else if (FORBIDDEN_WIRE_ALIAS.matcher(name).matches()) {
                violations.add(at(source, matcher.start(1),
                    "packet name retains forbidden alias: " + name));
            }
        }
    }

    private static void requireSuffix(
        SourceFile source,
        int offset,
        String type,
        String suffix,
        String handler,
        List<String> violations) {
        if (!type.endsWith(suffix)) {
            violations.add(at(source, offset,
                handler + " type " + type + " must end in " + suffix));
        }
    }

    private static String expressionType(String expression, String content, int before) {
        String compact = expression.strip();
        Matcher envelope = Pattern.compile(
            "^(?:[A-Za-z_][A-Za-z0-9_]*\\.)*ZLinkMessage\\.of\\s*\\(")
            .matcher(compact);
        if (envelope.find()) {
            int open = envelope.end() - 1;
            int close = matchingParen(compact, open);
            if (close > open) {
                return expressionType(compact.substring(open + 1, close), content, before);
            }
        }
        Matcher inline = Pattern.compile(
            "(?:new\\s+)?(?:[A-Za-z_][A-Za-z0-9_]*\\.)*([A-Z][A-Za-z0-9_]*)\\s*\\(")
            .matcher(compact);
        while (inline.find()) {
            String name = inline.group(1);
            if (!name.equals("ZLinkMessage") && !name.equals("RoutingId")
                    && !name.equals("Duration") && !name.equals("URI")) {
                return name;
            }
        }
        Matcher identifier = Pattern.compile("^([A-Za-z_][A-Za-z0-9_]*)$").matcher(compact);
        if (!identifier.matches()) {
            return null;
        }
        return inferVariableType(content, identifier.group(1), before);
    }

    private static String inferVariableType(String content, String variable, int before) {
        String prefix = content.substring(0, Math.max(0, before));
        List<Pattern> patterns = List.of(
            Pattern.compile("(?:[A-Za-z_][A-Za-z0-9_]*\\.)*([A-Z][A-Za-z0-9_]*)"
                + "\\s+" + Pattern.quote(variable) + "\\b"),
            Pattern.compile(Pattern.quote(variable) + "\\s*:\\s*"
                + "(?:[A-Za-z_][A-Za-z0-9_]*\\.)*([A-Z][A-Za-z0-9_]*)"),
            Pattern.compile("(?:val|var)\\s+" + Pattern.quote(variable)
                + "\\s*=.{0,240}?<\\s*(?:[A-Za-z_][A-Za-z0-9_]*\\.)*"
                + "([A-Z][A-Za-z0-9_]*)\\s*>",
                Pattern.DOTALL),
            Pattern.compile("(?:val|var)\\s+" + Pattern.quote(variable)
                + "\\s*=.{0,240}?(?:[A-Za-z_][A-Za-z0-9_]*\\.)*"
                + "([A-Z][A-Za-z0-9_]*(?:Req|Res|Msg|Notify|Event))::class\\.java",
                Pattern.DOTALL),
            Pattern.compile("(?:val|var)\\s+" + Pattern.quote(variable)
                + "\\s*=\\s*(?:ZLinkMessage\\.of\\s*\\(\\s*)?(?:new\\s+)?"
                + "(?:[A-Za-z_][A-Za-z0-9_]*\\.)*([A-Z][A-Za-z0-9_]*)\\s*\\(",
                Pattern.DOTALL));
        String result = null;
        int resultOffset = -1;
        for (Pattern pattern : patterns) {
            Matcher matcher = pattern.matcher(prefix);
            while (matcher.find()) {
                if (matcher.start() >= resultOffset) {
                    result = matcher.group(1);
                    resultOffset = matcher.start();
                }
            }
        }
        return result;
    }

    private static int createChainEnd(String content, int from) {
        int max = Math.min(content.length(), from + 3000);
        int submit = content.indexOf(".submit", from);
        int await = content.indexOf(".await", from);
        int end = max;
        if (submit >= 0 && submit < end) {
            end = Math.min(max, submit + 200);
        }
        if (await >= 0 && await < end) {
            end = Math.min(max, await + 200);
        }
        return end;
    }

    private static int matchingParen(String content, int open) {
        int depth = 0;
        boolean string = false;
        boolean character = false;
        boolean escaped = false;
        for (int index = open; index < content.length(); index++) {
            char value = content.charAt(index);
            if (escaped) {
                escaped = false;
                continue;
            }
            if ((string || character) && value == '\\') {
                escaped = true;
                continue;
            }
            if (!character && value == '"') {
                string = !string;
                continue;
            }
            if (!string && value == '\'') {
                character = !character;
                continue;
            }
            if (string || character) {
                continue;
            }
            if (value == '(') {
                depth++;
            } else if (value == ')' && --depth == 0) {
                return index;
            }
        }
        return -1;
    }

    private static List<String> topLevelArguments(String content) {
        List<String> arguments = new ArrayList<>();
        int start = 0;
        int round = 0;
        int angle = 0;
        int square = 0;
        int brace = 0;
        boolean string = false;
        boolean escaped = false;
        for (int index = 0; index < content.length(); index++) {
            char value = content.charAt(index);
            if (escaped) {
                escaped = false;
                continue;
            }
            if (string && value == '\\') {
                escaped = true;
                continue;
            }
            if (value == '"') {
                string = !string;
                continue;
            }
            if (string) {
                continue;
            }
            switch (value) {
                case '(' -> round++;
                case ')' -> round--;
                case '<' -> angle++;
                case '>' -> angle = Math.max(0, angle - 1);
                case '[' -> square++;
                case ']' -> square--;
                case '{' -> brace++;
                case '}' -> brace--;
                case ',' -> {
                    if (round == 0 && angle == 0 && square == 0 && brace == 0) {
                        arguments.add(content.substring(start, index).strip());
                        start = index + 1;
                    }
                }
                default -> { }
            }
        }
        String tail = content.substring(start).strip();
        if (!tail.isEmpty()) {
            arguments.add(tail);
        }
        return arguments;
    }

    private static String simpleType(String text) {
        Matcher matcher = TYPE_TOKEN.matcher(text.strip());
        String result = "";
        while (matcher.find()) {
            result = matcher.group(1);
        }
        return result;
    }

    private static boolean isContractDeclarationSource(Path path) {
        String name = path.getFileName().toString();
        return name.equals("Contracts.java")
            || name.equals("Contracts.kt")
            || name.equals("Messages.kt")
            || name.endsWith(".proto");
    }

    private static List<SourceFile> sources() throws IOException {
        List<SourceFile> result = new ArrayList<>();
        for (Path root : List.of(javaRoot().resolve("e2e"), javaRoot().resolve("e2e-kotlin"))) {
            try (Stream<Path> files = Files.walk(root)) {
                files.filter(Files::isRegularFile)
                    .filter(E2EWireNamingContractTest::isE2eSource)
                    .forEach(path -> result.add(new SourceFile(path, read(path))));
            }
        }
        return result;
    }

    private static boolean isE2eSource(Path path) {
        String normalized = path.toString().replace('\\', '/');
        return !normalized.contains("/build/")
            && !normalized.contains("/.gradle/")
            && (normalized.endsWith(".java")
                || normalized.endsWith(".kt")
                || normalized.endsWith(".proto"));
    }

    private static String read(Path path) {
        try {
            return Files.readString(path);
        } catch (IOException error) {
            throw new IllegalStateException("failed to read " + path, error);
        }
    }

    private static String at(SourceFile source, int offset, String message) {
        long line = source.content().substring(0, Math.max(0, offset)).lines().count();
        return source.path() + ":" + Math.max(1, line) + ": " + message;
    }

    private static Path javaRoot() {
        return Path.of(System.getProperty("user.dir")).getParent();
    }

    private record SourceFile(Path path, String content) { }
}
