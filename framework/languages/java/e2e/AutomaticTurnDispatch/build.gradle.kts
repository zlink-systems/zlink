// Config 8 AutomaticTurnDispatch build isolation.
val e2eBuildDir = providers.gradleProperty("zlinkE2eBuildDir").orNull
if (!e2eBuildDir.isNullOrBlank()) {
    layout.buildDirectory.set(file(e2eBuildDir))
}

subprojects {
    val rootE2eBuildDir = e2eBuildDir
    if (!rootE2eBuildDir.isNullOrBlank()) {
        layout.buildDirectory.set(file("${rootE2eBuildDir}/${project.path.removePrefix(":").replace(":", "-")}"))
    }

    plugins.withType<JavaPlugin> {
        extensions.configure<JavaPluginExtension> {
            toolchain {
                languageVersion.set(JavaLanguageVersion.of(22))
            }
        }
    }
}
