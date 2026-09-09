import org.gradle.api.plugins.JavaPlugin
import org.gradle.api.plugins.JavaPluginExtension
import org.gradle.jvm.toolchain.JavaLanguageVersion

val zlinkJavaLanguageVersion = 25

pluginManagement {
    plugins {
        id("org.jetbrains.kotlin.jvm") version "2.3.21"
        id("org.jetbrains.kotlin.plugin.spring") version "2.3.21"
    }
}

if (!gradle.extensions.extraProperties.has("zlink.jvmBaselineConfigured")) {
    gradle.extensions.extraProperties["zlink.jvmBaselineConfigured"] = true
    gradle.extensions.extraProperties["zlink.javaLanguageVersion"] = zlinkJavaLanguageVersion

    gradle.beforeProject {
        plugins.withType<JavaPlugin> {
            extensions.configure<JavaPluginExtension> {
                toolchain.languageVersion.set(
                    JavaLanguageVersion.of(zlinkJavaLanguageVersion),
                )
            }
        }
    }
}
