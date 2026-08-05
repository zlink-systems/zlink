plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("io.micrometer:micrometer-core:1.15.8")
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    implementation(kotlin("stdlib"))
}

kotlin { jvmToolchain(22) }

application {
    applicationName = "observability-ops-trigger"
    mainClass.set("systems.zlink.e2e.kotlin.observabilityops.trigger.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
