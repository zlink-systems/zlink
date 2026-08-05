package systems.zlink.framework;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;

final class JvmPublicContractSourceOwnerTest {
    private static final Set<String> SERVER_KEYS = Set.of("java", "kotlin");
    private static final Set<String> CLIENT_KEYS = Set.of(
        "javaHttp", "kotlinHttp", "streamConnector", "codecExtensions");

    @Test
    void sourceOwnerInventoryMatchesLiveJvmModulesAndSources() throws Exception {
        Path root = repositoryRoot();
        Path inventoryPath = root.resolve(
            "framework/doc/contract-inventory/jvm-public-contract-source-owners.json");
        JsonNode inventory = new ObjectMapper().readTree(
            Files.readString(inventoryPath, StandardCharsets.UTF_8));

        assertEquals(1, inventory.path("schemaVersion").asInt());
        assertFieldNames(
            inventory.path("apiSnapshots"),
            Set.of("java", "kotlin"));
        for (JsonNode snapshot : inventory.path("apiSnapshots")) {
            assertTrue(
                Files.isRegularFile(root.resolve(snapshot.asText())),
                "API snapshot artifact is missing: " + snapshot.asText());
        }
        assertTrue(inventory.path("providerPublicOwners").isArray());
        assertFieldNames(inventory.path("serverArtifacts"), SERVER_KEYS);
        assertFieldNames(inventory.path("clientArtifacts"), CLIENT_KEYS);

        JsonNode server = inventory.path("serverArtifacts");
        assertJvmOwner(root, "serverArtifacts.java", server.path("java"));
        assertJvmOwner(root, "serverArtifacts.kotlin", server.path("kotlin"));

        JsonNode clients = inventory.path("clientArtifacts");
        assertJvmOwner(root, "clientArtifacts.javaHttp", clients.path("javaHttp"));
        assertJvmOwner(root, "clientArtifacts.kotlinHttp", clients.path("kotlinHttp"));
        assertJvmOwner(root, "clientArtifacts.streamConnector", clients.path("streamConnector"));

        JsonNode codecs = clients.path("codecExtensions");
        assertCommonOwner(root, "clientArtifacts.codecExtensions", codecs);
        assertTrue(codecs.path("artifacts").isArray());
        for (JsonNode artifact : codecs.path("artifacts")) {
            assertArtifactProject(root, "clientArtifacts.codecExtensions", artifact.asText());
        }
        assertPublicOwners(root, "clientArtifacts.codecExtensions", codecs, codecs.path("artifacts"));
    }

    private static void assertJvmOwner(Path root, String ownerName, JsonNode owner) {
        assertCommonOwner(root, ownerName, owner);
        JsonNode artifacts = owner.has("artifacts")
            ? owner.path("artifacts")
            : owner.path("artifact").isTextual()
                ? new ObjectMapper().createArrayNode().add(owner.path("artifact").asText())
                : null;
        assertTrue(artifacts != null && artifacts.isArray(), ownerName + " must declare artifacts");
        for (JsonNode artifact : artifacts) {
            assertArtifactProject(root, ownerName, artifact.asText());
        }
        assertPublicOwners(root, ownerName, owner, artifacts);

        if (owner.path("module").isTextual()) {
            String artifact = artifacts.get(0).asText();
            String project = artifact.substring(artifact.indexOf(':') + 1);
            Path moduleInfo = root.resolve("framework/languages/java")
                .resolve(project)
                .resolve("src/main/java/module-info.java");
            assertTrue(Files.isRegularFile(moduleInfo), ownerName + " module-info missing");
            String moduleDeclaration = "module\\s+"
                + Pattern.quote(owner.path("module").asText())
                + "\\s*\\{";
            assertTrue(
                Pattern.compile(moduleDeclaration).matcher(read(moduleInfo)).find(),
                ownerName + " module declaration mismatch");
        }
    }

    private static void assertCommonOwner(Path root, String ownerName, JsonNode owner) {
        assertTrue(owner.isObject(), ownerName + " must be an object");
        assertTrue(owner.path("boundaryGate").isTextual()
            && !owner.path("boundaryGate").asText().isBlank(),
            ownerName + " boundary gate missing");
        JsonNode contract = owner.path("contractSourceOwner");
        if (contract.isTextual()) {
            assertTrue(Files.isRegularFile(root.resolve(contract.asText())),
                ownerName + " contract source owner missing");
        }
        JsonNode sourceOwner = owner.path("sourceOwner");
        if (sourceOwner.isTextual()) {
            assertTrue(Files.isRegularFile(root.resolve(
                "framework/languages/java").resolve(sourceOwner.asText())),
                ownerName + " Kotlin source owner missing");
        }
        if (owner.path("runtimeInternalPrefix").isTextual()) {
            assertFalse(owner.path("publicPackage").asText("")
                .startsWith(owner.path("runtimeInternalPrefix").asText()));
        }
    }

    private static void assertArtifactProject(Path root, String ownerName, String artifact) {
        assertTrue(artifact.startsWith("systems.zlink:"), ownerName + " artifact group mismatch");
        String project = artifact.substring(artifact.indexOf(':') + 1);
        assertTrue(Files.isDirectory(root.resolve("framework/languages/java").resolve(project)),
            ownerName + " artifact project missing: " + project);
        String settings = read(root.resolve("framework/languages/java/settings.gradle.kts"));
        assertTrue(settings.contains("\"" + project + "\""),
            ownerName + " artifact is not included: " + project);
    }

    private static void assertPublicOwners(
        Path root,
        String ownerName,
        JsonNode owner,
        JsonNode artifacts) {
        JsonNode publicOwners = owner.path("publicOwners");
        if (publicOwners.isArray()) {
            for (JsonNode publicOwner : publicOwners) {
                String packagePath = publicOwner.asText().replace('.', '/');
                boolean found = false;
                for (JsonNode artifact : artifacts) {
                    String project = artifact.asText().substring(artifact.asText().indexOf(':') + 1);
                    Path source = root.resolve("framework/languages/java")
                        .resolve(project)
                        .resolve("src/main/java")
                        .resolve(packagePath);
                    if (Files.isDirectory(source)) {
                        found = true;
                        break;
                    }
                }
                assertTrue(found, ownerName + " public owner missing: " + publicOwner.asText());
            }
        }
        JsonNode publicPackage = owner.path("publicPackage");
        if (publicPackage.isTextual()) {
            String packagePath = publicPackage.asText().replace('.', '/');
            boolean found = false;
            for (JsonNode artifact : artifacts) {
                String project = artifact.asText().substring(artifact.asText().indexOf(':') + 1);
                Path javaSource = root.resolve("framework/languages/java")
                    .resolve(project).resolve("src/main/java").resolve(packagePath);
                Path kotlinSource = root.resolve("framework/languages/java")
                    .resolve(project).resolve("src/main/kotlin").resolve(packagePath);
                if (Files.isDirectory(javaSource) || Files.isDirectory(kotlinSource)) {
                    found = true;
                    break;
                }
            }
            assertTrue(found, ownerName + " public package missing: " + publicPackage.asText());
        }
    }

    private static void assertFieldNames(JsonNode object, Set<String> expected) {
        Set<String> actual = new HashSet<>();
        Iterator<String> names = object.fieldNames();
        names.forEachRemaining(actual::add);
        assertEquals(expected, actual);
    }

    private static String read(Path path) {
        try {
            return Files.readString(path, StandardCharsets.UTF_8);
        } catch (IOException error) {
            throw new java.io.UncheckedIOException(error);
        }
    }

    private static Path repositoryRoot() {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            if (Files.isRegularFile(current.resolve("framework/languages/java/settings.gradle.kts"))
                && Files.isDirectory(current.resolve("framework/doc/framework/common/spec"))) {
                return current;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("repository root not found");
    }
}
