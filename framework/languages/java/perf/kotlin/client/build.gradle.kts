plugins {
    application
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":shared"))
    implementation(project(":client"))
    implementation(project(":kotlin:shared"))
    implementation(zlinkLibs.zlink.stream.connector)
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

application {
    applicationName = "client"
    mainClass.set("systems.zlink.framework.perf.client.ProgramKt")
    applicationDefaultJvmArgs = listOf("--enable-native-access=ALL-UNNAMED")
}

tasks.withType<Jar>().configureEach { archiveBaseName.set("kotlin-${project.name}") }
