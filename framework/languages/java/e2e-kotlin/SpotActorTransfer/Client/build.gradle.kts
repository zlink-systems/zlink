plugins {
    application
    kotlin("jvm")
}

dependencies {
    implementation(project(":Shared"))
    implementation(project(":JavaClient"))
    implementation("systems.zlink:zlink-framework-kotlin:0.9.0")
    implementation("systems.zlink:zlink-stream-connector:0.9.0")
    implementation(zlinkLibs.zlink.bindings)
    implementation("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2")
}

application {
    applicationName = "spot-actor-transfer-kotlin-client"
    mainClass.set("systems.zlink.e2e.kotlin.spotactortransfer.client.ProgramKt")
}
