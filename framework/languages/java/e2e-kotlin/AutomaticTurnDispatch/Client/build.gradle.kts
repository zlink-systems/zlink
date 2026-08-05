plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "automatic-turn-dispatch-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.automaticturn.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
