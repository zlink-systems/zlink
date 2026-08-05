plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "resilience-lifecycle-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.resiliencelifecycle.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
