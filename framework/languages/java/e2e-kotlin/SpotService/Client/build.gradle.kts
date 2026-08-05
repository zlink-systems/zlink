plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(22)
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-framework-kotlin:0.1.0-SNAPSHOT")
    implementation("systems.zlink:zlink-stream-connector:0.1.0-SNAPSHOT")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.19.2")
}

application {
    applicationName = "spot-service-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.spotservice.client.ClientProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
