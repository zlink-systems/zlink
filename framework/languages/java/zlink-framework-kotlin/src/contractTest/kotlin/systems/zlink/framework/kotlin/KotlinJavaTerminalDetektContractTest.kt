package systems.zlink.framework.kotlin

import java.lang.reflect.Modifier
import java.net.URI
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.CompletionStage
import java.util.jar.JarFile
import kotlin.io.path.readText
import org.junit.jupiter.api.Assertions.assertEquals
import org.junit.jupiter.api.Assertions.assertThrows
import org.junit.jupiter.api.Test
import systems.zlink.framework.configuration.ZLinkFrameworkOptions

class KotlinJavaTerminalDetektContractTest {
    private val javaRoot =
        generateSequence(Path.of("").toAbsolutePath()) { it.parent }
            .first { Files.isRegularFile(it.resolve(DETEKT_CONFIG)) }
    private val specRoot =
        javaRoot.resolve("../../doc/framework/common/spec/server/languages/kotlin/interfaces")

    @Test
    fun forbiddenMethodConfigurationMatchesTheKotlinContractSurfaces() {
        assertContractMatches(readSpecs(), configuredMethods())
    }

    @Test
    fun addingASpecMemberMakesTheContractFail() {
        val specs = readSpecs().toMutableMap()
        specs["stream-session.ko.md"] =
            specs
                .getValue("stream-session.ko.md")
                .replace(
                    "interface ZLinkKotlinSessionActor {",
                    """
                interface ZLinkKotlinSessionActor {
                 fun notifyDisconnected(): ZLinkKotlinSubmissionCall
                """
                        .trimIndent(),
                )

        assertThrows(AssertionError::class.java) {
            assertContractMatches(specs, configuredMethods())
        }
    }

    @Test
    fun removingAConfiguredMethodMakesTheContractFail() {
        val configured = configuredMethods()

        assertThrows(AssertionError::class.java) {
            assertContractMatches(readSpecs(), configured.drop(1).toSet())
        }
    }

    private fun assertContractMatches(specs: Map<String, String>, configured: Set<String>) {
        assertEquals(expectedMethods(specs), configured)
    }

    private fun expectedMethods(specs: Map<String, String>): Set<String> {
        val apiClasses = publicJavaApiClasses()
        val expected = mutableSetOf<String>()

        specs.forEach { (file, document) ->
            val source = kotlinSourceSignatures(document)
            expected += registrationSurfaces(source, apiClasses)
            expected += terminalSurfaces(file, source, apiClasses)
        }
        return expected
    }

    private fun registrationSurfaces(source: String, apiClasses: Set<Class<*>>): Set<String> =
        reifiedExtensionMembers(source).mapNotNullTo(mutableSetOf()) { (receiver, name) ->
            apiClasses
                .singleOrNull { type ->
                    type.simpleName == receiver &&
                        type.methods.any { method ->
                            method.name == name &&
                                method.parameterTypes.any { it == Class::class.java }
                        }
                }
                ?.let { "${it.name}.$name" }
        }

    private fun terminalSurfaces(
        file: String,
        source: String,
        apiClasses: Set<Class<*>>,
    ): Set<String> {
        val packageName = SPEC_API_PACKAGES[file] ?: return emptySet()
        val terminalNames = mutableSetOf<String>()
        if (Regex("""\bsuspend\s+fun\s+await\s*\(""").containsMatchIn(source)) {
            terminalNames += "submit"
        }
        if (Regex("""\bsuspend\s+fun\s+yield\s*\(""").containsMatchIn(source)) {
            terminalNames += "yield"
        }

        val packageTypes = apiClasses.filter { it.packageName == packageName }
        val result =
            packageTypes.flatMapTo(mutableSetOf()) { type ->
                terminalNames.mapNotNull { name ->
                    type.methods
                        .takeIf { methods -> methods.any { isStageMethod(it, name) } }
                        ?.let { "${type.name}.$name" }
                }
            }

        directSubmissionMembers(source).forEach { (kotlinOwner, name) ->
            val javaOwner = kotlinOwner.replaceFirst("ZLinkKotlin", "ZLink")
            packageTypes
                .singleOrNull { it.simpleName == javaOwner }
                ?.takeIf { type -> type.methods.any { isStageMethod(it, name) } }
                ?.let { result += "${it.name}.$name" }
        }
        return result
    }

    private fun isStageMethod(method: java.lang.reflect.Method, name: String): Boolean =
        method.name == name && CompletionStage::class.java.isAssignableFrom(method.returnType)

    private fun reifiedExtensionMembers(source: String): Set<Pair<String, String>> =
        Regex("""inline\s+fun\b""")
            .findAll(source)
            .mapNotNull { match ->
                val openingParenthesis = source.indexOf('(', match.range.last + 1)
                if (openingParenthesis < 0) return@mapNotNull null
                val declaration = source.substring(match.range.first, openingParenthesis)
                if (!Regex("""\breified\b""").containsMatchIn(declaration)) {
                    return@mapNotNull null
                }
                val receiver =
                    Regex("""([A-Z][A-Za-z0-9_]*)(?:<[^<>]*>)?\s*$""")
                        .find(declaration.substringBeforeLast('.'))
                        ?.groupValues
                        ?.get(1)
                val name = Regex("""\w+\s*$""").find(declaration)?.value?.trim()
                if (receiver != null && name != null) receiver to name else null
            }
            .toSet()

    private fun directSubmissionMembers(source: String): Set<Pair<String, String>> =
        Regex("""interface\s+(ZLinkKotlin\w+)\s*(?:<[^{}]*>)?\s*\{([\s\S]*?)\n}""")
            .findAll(source)
            .flatMap { interfaceMatch ->
                val owner = interfaceMatch.groupValues[1]
                Regex("""\bfun\s+(\w+)\s*\([\s\S]*?\)\s*:\s*ZLinkKotlinSubmissionCall\b""")
                    .findAll(interfaceMatch.groupValues[2])
                    .map { owner to it.groupValues[1] }
            }
            .toSet()

    private fun kotlinSourceSignatures(document: String): String {
        val section = document.substringAfter("## Kotlin source signature")
        val beforeGenerated = section.substringBefore("## generated JVM signature")
        return Regex("""```kotlin\s*([\s\S]*?)```""").findAll(beforeGenerated).joinToString("\n") {
            it.groupValues[1]
        }
    }

    private fun configuredMethods(): Set<String> =
        Regex("'([^']+)'")
            .findAll(javaRoot.resolve(DETEKT_CONFIG).readText())
            .map { it.groupValues[1] }
            .toSet()

    private fun readSpecs(): Map<String, String> =
        SPEC_API_PACKAGES.keys.associateWith { specRoot.resolve(it).readText() }

    private fun publicJavaApiClasses(): Set<Class<*>> {
        val location =
            ZLinkFrameworkOptions::class.java.protectionDomain.codeSource.location.toURI()
        return classNames(location)
            .asSequence()
            .filter { name ->
                SPEC_API_PACKAGES.values.any { name.startsWith("$it.") } && '$' !in name
            }
            .map { Class.forName(it, false, ZLinkFrameworkOptions::class.java.classLoader) }
            .filter { Modifier.isPublic(it.modifiers) }
            .toSet()
    }

    private fun classNames(location: URI): Set<String> {
        val path = Path.of(location)
        if (Files.isDirectory(path)) {
            return Files.walk(path).use { paths ->
                paths
                    .filter { it.toString().endsWith(".class") }
                    .map { path.relativize(it).toString().replace('\\', '.').replace('/', '.') }
                    .map { it.removeSuffix(".class") }
                    .toList()
                    .toSet()
            }
        }
        return JarFile(path.toFile()).use { jar ->
            jar.entries()
                .asSequence()
                .map { it.name }
                .filter { it.endsWith(".class") }
                .map { it.removeSuffix(".class").replace('/', '.') }
                .toSet()
        }
    }

    private companion object {
        const val DETEKT_CONFIG = "config/detekt/kotlin-java-terminals.yml"
        val SPEC_API_PACKAGES =
            mapOf(
                "configuration-host.ko.md" to "systems.zlink.framework.configuration",
                "channel-messaging.ko.md" to "systems.zlink.framework.channels",
                "spots.ko.md" to "systems.zlink.framework.spots",
                "actors.ko.md" to "systems.zlink.framework.actors",
                "stream-session.ko.md" to "systems.zlink.framework.streams",
            )
    }
}
