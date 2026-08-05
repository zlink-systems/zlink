plugins {
    `java-library`
    `maven-publish`
    jacoco
    id("org.jetbrains.kotlin.jvm")
}

description = "ZLink Kotlin HTTP client coroutine (suspend) and DSL extensions"

// Follows the java client version (single source: HttpClientVersion.java).
version = Regex("VERSION = \"([^\"]+)\"")
    .find(file("../zlink-http-client/src/main/java/systems/zlink/httpclient/internal/HttpClientVersion.java").readText())!!
    .groupValues[1]

kotlin {
    jvmToolchain(22)
}

dependencies {
    api(project(":zlink-http-client"))
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    api("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    // Lets the shared Jackson mapper (findAndAddModules) deserialize Kotlin data classes.
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}

tasks.named<Test>("test") {
    finalizedBy(tasks.named("jacocoTestReport"))
}

tasks.named<JacocoReport>("jacocoTestReport") {
    dependsOn(tasks.named("test"))
    reports {
        xml.required.set(true)
        html.required.set(true)
    }
}

tasks.named<JacocoCoverageVerification>("jacocoTestCoverageVerification") {
    dependsOn(tasks.named("test"))
    violationRules {
        rule {
            limit {
                counter = "LINE"
                minimum = "0.80".toBigDecimal()
            }
        }
    }
}

tasks.named("check") {
    dependsOn(tasks.named("jacocoTestCoverageVerification"))
}

// The shared publishing config stamps the publication version before this script runs;
// restate it so the published artifact uses the module version above.
publishing {
    publications.withType<MavenPublication>().configureEach {
        version = project.version.toString()
    }
}
