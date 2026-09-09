plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
}


application {
    applicationName = "runtime-monitoring-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.runtimemonitoring.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
