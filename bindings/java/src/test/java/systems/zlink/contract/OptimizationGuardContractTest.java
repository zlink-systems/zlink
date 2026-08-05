package systems.zlink.contract;



import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.lang.reflect.GenericArrayType;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;
import javax.tools.ToolProvider;
import org.junit.jupiter.api.Test;

final class OptimizationGuardContractTest {
    private static final Path MAIN_SOURCE = Path.of("src", "main", "java");
    private static final Path MODULE_INFO =
        MAIN_SOURCE.resolve("module-info.java");
    private static final Path MAIN_CLASSES =
        Path.of("build", "classes", "java", "main");
    private static final Path SAMPLES_SOURCE = Path.of("samples");
    private static final Path KOTLIN_SAMPLES_SOURCE =
        Path.of("..", "kotlin", "samples", "src", "main", "kotlin");
    private static final Path PERF_SOURCE = Path.of("perf");
    private static final String ZLINK_FACTORY_RUNTIME_IMPORT =
        "import systems.zlink.runtime.nativeapi.ContractAccess;";
    private static final String RUNTIME_PREFIX = "systems.zlink.runtime.";
    private static final String INTERNAL_PREFIX = "systems.zlink.internal.";

    private static final String[] AGGREGATE_SYMBOLS = {
        "zlink_send",
        "zlink_recv",
        "zlink_publish",
        "zlink_subscribe",
        "zlink_router_recv",
        "zlink_dealer_request",
        "zlink_router_request",
        "zlink_router_reply"
    };

    private static final String[] REQUIRED_PART_SYMBOLS = {
        "zlink_send_part",
        "zlink_recv_part",
        "zlink_publish_part",
        "zlink_subscribe_part",
        "zlink_router_recv_part",
        "zlink_dealer_request_part",
        "zlink_router_request_part",
        "zlink_router_reply_part"
    };

    @Test
    void hotPathsUsePartSubstrateInsteadOfAggregateSymbols() throws IOException {
        String source = allMainSource();

        for (String symbol : REQUIRED_PART_SYMBOLS) {
            assertTrue(source.contains(symbol), "missing helper substrate " + symbol);
        }

        List<String> violations = new ArrayList<>();
        for (String symbol : AGGREGATE_SYMBOLS) {
            Pattern exactSymbolString = Pattern.compile("\"" + Pattern.quote(symbol) + "\"");
            if (exactSymbolString.matcher(source).find()) {
                violations.add(symbol);
            }
        }

        assertTrue(violations.isEmpty(), "aggregate hot-path symbols found: " + violations);
    }

    @Test
    void runtimeSourceDoesNotUseReflectionOrNativeAccessWorkarounds() throws IOException {
        String source = allMainSource();
        String sourceWithoutUnsafeBootstrap = source
            .replace("Unsafe.class.getDeclaredField(\"theUnsafe\")", "")
            .replace("field.setAccessible(true)", "");

        assertFalse(sourceWithoutUnsafeBootstrap.contains("setAccessible(true)"));
        assertFalse(sourceWithoutUnsafeBootstrap.contains("getDeclaredMethod("));
        assertFalse(sourceWithoutUnsafeBootstrap.contains("getDeclaredField("));
        assertFalse(sourceWithoutUnsafeBootstrap.contains("Class.forName(\"com.sun.jna"));
        assertFalse(sourceWithoutUnsafeBootstrap.contains("com.sun.jna"));
    }

    @Test
    void moduleExportsOnlyPublicContractPackages() throws IOException {
        List<String> violations = new ArrayList<>();
        for (String line : Files.readAllLines(MODULE_INFO, StandardCharsets.UTF_8)) {
            String trimmed = line.trim();
            if ((trimmed.startsWith("exports systems.zlink.")
                    && !trimmed.startsWith("exports systems.zlink.contracts."))
                || trimmed.startsWith("opens systems.zlink.")) {
                violations.add(trimmed);
            }
        }

        assertTrue(violations.isEmpty(),
            "non-contract packages exposed from Java module: " + violations);
    }

    @Test
    void sourceTopLevelPackagesStayWithinDeclaredOwners() throws IOException {
        Path zlinkPackage = MAIN_SOURCE.resolve(Path.of("systems", "zlink"));
        Set<String> allowed = Set.of("contracts", "internal", "runtime");
        List<String> violations = new ArrayList<>();

        try (var stream = Files.list(zlinkPackage)) {
            for (Path path : stream.filter(Files::isDirectory).toList()) {
                String name = path.getFileName().toString();
                if (!allowed.contains(name)) {
                    violations.add(name);
                }
            }
        }

        assertTrue(violations.isEmpty(),
            "top-level Java packages must stay within declared owners: " + violations);
    }

    @Test
    void contractSourcesReferenceRuntimeOnlyThroughExactZlinkFactoryImport()
        throws IOException {
        Path contracts = MAIN_SOURCE.resolve(Path.of(
            "systems", "zlink", "contracts"));
        Path allowedFacade = contracts.resolve(Path.of("core", "Zlink.java"));
        List<String> violations = new ArrayList<>();

        for (Path path : sourceFiles(contracts)) {
            String source = Files.readString(path, StandardCharsets.UTF_8);
            violations.addAll(contractRuntimeReferenceViolations(
                path, allowedFacade, source));
        }

        assertTrue(violations.isEmpty(),
            "contract source must not reference runtime implementation: "
                + violations);
    }

    @Test
    void zlinkFactoryFacadeDoesNotExposeRuntimeOrInternalTypes() {
        List<String> violations = publicSignatureViolations(
            systems.zlink.contracts.core.Zlink.class);

        assertTrue(violations.isEmpty(),
            "Zlink factory facade must expose contract types only: "
                + violations);
    }

    @Test
    void sourceAndGenericSignatureGuardsRejectMutations() throws Exception {
        Path contracts = MAIN_SOURCE.resolve(Path.of(
            "systems", "zlink", "contracts"));
        Path allowedFacade = contracts.resolve(Path.of("core", "Zlink.java"));
        String source = Files.readString(allowedFacade, StandardCharsets.UTF_8);

        assertTrue(contractRuntimeReferenceViolations(
            allowedFacade, allowedFacade, source).isEmpty());
        assertFalse(contractRuntimeReferenceViolations(
            allowedFacade, allowedFacade,
            source + "\nimport systems.zlink.runtime.core.NativeContext;\n")
            .isEmpty());
        assertFalse(contractRuntimeReferenceViolations(
            allowedFacade, allowedFacade,
            source.replace(ZLINK_FACTORY_RUNTIME_IMPORT,
                "import systems.zlink.runtime.nativeapi.*;"))
            .isEmpty());
        assertFalse(contractRuntimeReferenceViolations(
            allowedFacade, allowedFacade,
            source + "\n// " + RUNTIME_PREFIX + "core.NativeContext\n")
            .isEmpty());

        assertFalse(publicSignatureViolations(
            RuntimeGenericSignatureMutation.class).isEmpty());
        assertFalse(publicSignatureViolations(
            InternalGenericSignatureMutation.class).isEmpty());
    }

    @Test
    void modulePathConsumersCanCompileAgainstContractOnly() throws IOException {
        String publicContract = """
            import systems.zlink.contracts.messaging.Message;
            final class PublicContractProbe {
                Message message;
            }
            """;
        assertTrue(compilesOnModulePath(publicContract),
            "module path consumers must be able to import public contract packages");

        List<String> nonContractSources = List.of(
            """
            import systems.zlink.runtime.core.NativeContext;
            final class RuntimeProbe {
                NativeContext context;
            }
            """,
            """
            import systems.zlink.internal.ContractAccess;
            final class InternalProbe {
                ContractAccess access;
            }
            """,
            """
            import systems.zlink.internal.sockets.SocketOptions;
            final class RuntimeSocketOptionsProbe {
                Object options = SocketOptions.all();
            }
            """
        );
        List<String> violations = new ArrayList<>();
        for (String source : nonContractSources) {
            if (compilesOnModulePath(source)) {
                violations.add(source.lines().filter(line -> line.startsWith("import "))
                    .findFirst().orElse(source));
            }
        }

        assertTrue(violations.isEmpty(),
            "module path consumers must not compile against non-contract packages: "
                + violations);
    }

    @Test
    void samplesAndPerfUseOnlyPublicBindingContract() throws IOException {
        List<String> violations = new ArrayList<>();
        for (Path root : List.of(SAMPLES_SOURCE, KOTLIN_SAMPLES_SOURCE, PERF_SOURCE)) {
            for (Path path : sourceFiles(root)) {
                String source = Files.readString(path, StandardCharsets.UTF_8);
                if (source.contains("import systems.zlink.internal.")
                    || source.contains("import systems.zlink.runtime.")
                    || source.contains("systems.zlink.internal.")
                    || source.contains("systems.zlink.runtime.")
                    || source.contains("ContractAccess")) {
                    violations.add(path.toString());
                }
            }
        }

        assertTrue(violations.isEmpty(),
            "samples/perf must stay on public contract packages: " + violations);
    }

    @Test
    void bindingsSamplesStayOnCore11RawSocketBoundary() throws IOException {
        List<String> violations = new ArrayList<>();
        for (Path root : List.of(SAMPLES_SOURCE, KOTLIN_SAMPLES_SOURCE)) {
            for (Path path : sourceFiles(root)) {
                String source = Files.readString(path, StandardCharsets.UTF_8);
                if (source.contains("systems.zlink.contracts.service.")) {
                    violations.add(path.toString());
                }
            }
        }

        assertTrue(violations.isEmpty(),
            "Actor, Spot and session service examples belong to Framework samples: "
                + violations);
    }

    @Test
    void contractPackagesDoNotExposeInternalNamespace() {
        Path contractInternal = MAIN_SOURCE.resolve(Path.of(
            "systems", "zlink", "contracts", "internal"));

        assertFalse(Files.exists(contractInternal),
            "contract packages must not expose an internal namespace: "
                + contractInternal);
    }

    @Test
    void runtimeImplementationTypesStayPackagePrivateWhenPossible()
        throws IOException {
        List<Path> implementationTypes = List.of(
            MAIN_SOURCE.resolve(Path.of("systems", "zlink", "runtime",
                "core", "NativeContext.java")),
            MAIN_SOURCE.resolve(Path.of("systems", "zlink", "runtime",
                "sockets", "NativeDealerRequestSupport.java"))
        );
        List<String> violations = new ArrayList<>();

        for (Path path : implementationTypes) {
            String source = Files.readString(path, StandardCharsets.UTF_8);
            if (source.contains("public final class ")) {
                violations.add(path.toString());
            }
        }

        assertTrue(violations.isEmpty(),
            "runtime implementation types must stay package-private when no "
                + "public contract requires them: " + violations);
    }

    @Test
    void nativeSocketDescriptorsStayRuntimePrivate() throws IOException {
        Path contractInternalSockets = MAIN_SOURCE.resolve(Path.of(
            "systems", "zlink", "contracts", "internal", "sockets"));
        List<String> violations = new ArrayList<>();

        if (Files.exists(contractInternalSockets)) {
            violations.add(contractInternalSockets.toString());
        }

        for (String fileName : List.of("SendFlag.java", "ReceiveFlag.java")) {
            Path contractPath = contractInternalSockets.resolve(fileName);
            if (Files.exists(contractPath)) {
                violations.add(contractPath.toString());
            }
        }

        Path runtimeSockets = MAIN_SOURCE.resolve(Path.of(
            "systems", "zlink", "runtime", "sockets"));
        for (String fileName : List.of("SendFlag.java", "ReceiveFlag.java")) {
            Path runtimePath = runtimeSockets.resolve(fileName);
            String source = Files.readString(runtimePath, StandardCharsets.UTF_8);
            if (source.contains("public enum ")) {
                violations.add(runtimePath + " declares a public enum");
            }
        }

        assertTrue(violations.isEmpty(),
            "native socket descriptors must stay runtime-private: " + violations);
    }

    private static String allMainSource() throws IOException {
        StringBuilder builder = new StringBuilder();
        try (var stream = Files.walk(MAIN_SOURCE)) {
            for (Path path : stream.filter(p -> p.toString().endsWith(".java")).toList()) {
                builder.append(Files.readString(path, StandardCharsets.UTF_8)).append('\n');
            }
        }
        return builder.toString();
    }

    private static List<Path> sourceFiles(Path root) throws IOException {
        try (var stream = Files.walk(root)) {
            return stream.filter(p ->
                p.toString().endsWith(".java") || p.toString().endsWith(".kt")).toList();
        }
    }

    private static List<String> contractRuntimeReferenceViolations(
        Path path, Path allowedFacade, String source) {
        List<String> violations = new ArrayList<>();
        String sourceWithoutAllowedImport = source;
        if (path.equals(allowedFacade)) {
            int importCount = countOccurrences(source,
                ZLINK_FACTORY_RUNTIME_IMPORT);
            if (importCount != 1) {
                violations.add(path + " must contain exactly one approved "
                    + "runtime factory import, found " + importCount);
            }
            sourceWithoutAllowedImport = source.replace(
                ZLINK_FACTORY_RUNTIME_IMPORT, "");
        }
        if (sourceWithoutAllowedImport.contains(RUNTIME_PREFIX)) {
            violations.add(path + " contains an unapproved runtime reference");
        }
        return violations;
    }

    private static int countOccurrences(String source, String value) {
        int count = 0;
        int offset = 0;
        while ((offset = source.indexOf(value, offset)) >= 0) {
            count++;
            offset += value.length();
        }
        return count;
    }

    private static List<String> publicSignatureViolations(Class<?> type) {
        List<String> violations = new ArrayList<>();
        for (var method : type.getDeclaredMethods()) {
            if (!Modifier.isPublic(method.getModifiers())) {
                continue;
            }
            List<Type> signatureTypes = new ArrayList<>();
            signatureTypes.add(method.getGenericReturnType());
            signatureTypes.addAll(Arrays.asList(method.getGenericParameterTypes()));
            for (TypeVariable<?> variable : method.getTypeParameters()) {
                signatureTypes.addAll(Arrays.asList(variable.getBounds()));
            }
            if (signatureTypes.stream().anyMatch(signatureType ->
                    containsForbiddenPublicType(signatureType, new HashSet<>()))) {
                violations.add(method.toGenericString());
            }
        }
        return violations;
    }

    private static boolean containsForbiddenPublicType(Type type,
                                                       Set<Type> visited) {
        if (type == null || !visited.add(type)) {
            return false;
        }
        if (type instanceof Class<?> classType) {
            if (classType.isArray()) {
                return containsForbiddenPublicType(
                    classType.getComponentType(), visited);
            }
            return isForbiddenPublicTypeName(classType.getName());
        }
        if (type instanceof ParameterizedType parameterizedType) {
            if (containsForbiddenPublicType(parameterizedType.getRawType(),
                    visited)
                || containsForbiddenPublicType(parameterizedType.getOwnerType(),
                    visited)) {
                return true;
            }
            return Arrays.stream(parameterizedType.getActualTypeArguments())
                .anyMatch(argument -> containsForbiddenPublicType(
                    argument, visited));
        }
        if (type instanceof GenericArrayType arrayType) {
            return containsForbiddenPublicType(
                arrayType.getGenericComponentType(), visited);
        }
        if (type instanceof TypeVariable<?> variable) {
            return Arrays.stream(variable.getBounds()).anyMatch(bound ->
                containsForbiddenPublicType(bound, visited));
        }
        if (type instanceof WildcardType wildcard) {
            return Arrays.stream(wildcard.getUpperBounds()).anyMatch(bound ->
                    containsForbiddenPublicType(bound, visited))
                || Arrays.stream(wildcard.getLowerBounds()).anyMatch(bound ->
                    containsForbiddenPublicType(bound, visited));
        }
        return false;
    }

    private static boolean isForbiddenPublicTypeName(String name) {
        return name.startsWith(RUNTIME_PREFIX)
            || name.startsWith(INTERNAL_PREFIX);
    }

    private static final class RuntimeGenericSignatureMutation {
        public List<List<systems.zlink.runtime.nativeapi.ContractAccess>>
            leakedType() {
            return List.of();
        }
    }

    private static final class InternalGenericSignatureMutation {
        public List<? extends systems.zlink.internal.ContractAccess>
            leakedType() {
            return List.of();
        }
    }

    private static boolean compilesOnModulePath(String source) throws IOException {
        var compiler = ToolProvider.getSystemJavaCompiler();
        assertTrue(compiler != null, "tests must run on a JDK with javac");

        Path workDir = Files.createTempDirectory("zlink-java-module-guard");
        Path moduleDir = workDir.resolve("probe");
        Path sourceDir = moduleDir.resolve("probe");
        Path moduleInfo = moduleDir.resolve("module-info.java");
        Path sourceFile = sourceDir.resolve("Probe.java");
        Path outputDir = workDir.resolve("out");
        Files.createDirectories(sourceDir);
        Files.createDirectories(outputDir);
        Files.writeString(moduleInfo,
            "module probe { requires systems.zlink; }", StandardCharsets.UTF_8);
        Files.writeString(sourceFile, "package probe;\n" + source,
            StandardCharsets.UTF_8);

        List<String> args = Arrays.asList(
            "-d", outputDir.toString(),
            "--module-path", MAIN_CLASSES.toString(),
            "--module-source-path", workDir.toString(),
            "-m", "probe"
        );

        return compiler.run(null, null, null, args.toArray(String[]::new)) == 0;
    }
}
