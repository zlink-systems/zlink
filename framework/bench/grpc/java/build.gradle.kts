// with-grpc local bench, java row: one client module and three server modules
// (spec section 3). Nothing here aggregates: tables, medians, G5 and the section 7.2
// ratios belong to framework/bench/tools (plan section 4.1).

val benchBuildDir = providers.gradleProperty("zlinkBenchBuildDir").orNull
if (!benchBuildDir.isNullOrBlank()) {
    layout.buildDirectory.set(file(benchBuildDir))
}

subprojects {
    val rootBuildDir = benchBuildDir
    if (!rootBuildDir.isNullOrBlank()) {
        layout.buildDirectory.set(
            file("$rootBuildDir/${project.path.removePrefix(":").replace(":", "-")}"))
    }

    plugins.withType<JavaPlugin> {
        extensions.configure<JavaPluginExtension> {
            toolchain {
                languageVersion.set(JavaLanguageVersion.of(22))
            }
        }
        tasks.withType<JavaCompile>().configureEach {
            options.encoding = "UTF-8"
        }
    }
}
