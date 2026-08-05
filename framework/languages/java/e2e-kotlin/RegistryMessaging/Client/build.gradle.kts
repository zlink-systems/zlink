plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}

kotlin {
    jvmToolchain(22)
}

application {
    applicationName = "registry-messaging-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.registrymessaging.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}
