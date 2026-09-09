plugins {
    base
    id("org.jetbrains.kotlin.jvm") apply false
    id("org.jetbrains.kotlin.plugin.spring") apply false
}

val e2eBuildDir = providers.environmentVariable("ZLINK_KOTLIN_E2E_BUILD_DIR").orNull
if (!e2eBuildDir.isNullOrBlank()) {
    layout.buildDirectory.set(file(e2eBuildDir))
}

subprojects {
    val rootE2eBuildDir = e2eBuildDir
    if (!rootE2eBuildDir.isNullOrBlank()) {
        layout.buildDirectory.set(file("${rootE2eBuildDir}/${project.path.removePrefix(":").replace(":", "-")}"))
    }

}
