plugins {
    base
    id("org.jetbrains.kotlin.jvm") apply false
    id("org.jetbrains.kotlin.plugin.spring") apply false
}

subprojects {
    plugins.withType<JavaPlugin> {
        extensions.configure<JavaPluginExtension> {
            toolchain.languageVersion.set(JavaLanguageVersion.of(22))
        }
    }
}
