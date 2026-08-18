val crossLanguageBuildDir = providers.gradleProperty("zlinkCrossLanguageBuildDir").orNull
if (!crossLanguageBuildDir.isNullOrBlank()) {
    layout.buildDirectory.set(file(crossLanguageBuildDir))
}

subprojects {
    val rootBuildDir = crossLanguageBuildDir
    if (!rootBuildDir.isNullOrBlank()) {
        layout.buildDirectory.set(file("${rootBuildDir}/${project.path.removePrefix(":").replace(":", "-")}"))
    }

    plugins.withType<JavaPlugin> {
        extensions.configure<JavaPluginExtension> {
            toolchain {
                languageVersion.set(JavaLanguageVersion.of(22))
            }
        }
    }
}
