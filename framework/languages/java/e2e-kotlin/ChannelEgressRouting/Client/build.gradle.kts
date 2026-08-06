plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation("systems.zlink:zlink-framework-kotlin:0.10.0")
    implementation("systems.zlink:zlink-stream-connector:0.10.0")
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
}

application {
    applicationName = "channel-egress-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.channelegress.client.ClientApplicationKt")
}

kotlin.jvmToolchain(22)
