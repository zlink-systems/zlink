plugins {
    `java-library`
    `maven-publish`
    id("org.jetbrains.kotlin.jvm")
}

description = "ZLink Framework Kotlin coroutine and DSL extensions"

kotlin {
    jvmToolchain(22)
}

dependencies {
    api(project(":zlink-framework-core"))
    api(project(":zlink-stream-connector"))
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    api("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    testImplementation("systems.zlink:zlink")
    testImplementation(project(":zlink-framework-spring-boot-starter"))
    testImplementation(project(":zlink-framework-testkit"))
}
