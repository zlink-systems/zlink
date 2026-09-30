plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    implementation(project(":server-support"))
    implementation(zlinkLibs.zlink.framework.kotlin)
    implementation(zlinkLibs.zlink.bindings)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
}

tasks.withType<Jar>().configureEach { archiveBaseName.set("kotlin-${project.name}") }
