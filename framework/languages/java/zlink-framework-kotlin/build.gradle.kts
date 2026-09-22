import dev.detekt.gradle.Detekt

plugins {
    `java-library`
    `maven-publish`
    id("org.jetbrains.kotlin.jvm")
}

description = "ZLink Framework Kotlin coroutine and DSL extensions"


dependencies {
    api(project(":zlink-framework-core"))
    api(project(":zlink-stream-connector"))
    api("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    api("org.jetbrains.kotlinx:kotlinx-coroutines-jdk8:1.9.0")
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
    testImplementation("systems.zlink:zlink")
    testImplementation(project(":zlink-framework-spring-boot-starter"))
    testImplementation(project(":zlink-framework-testkit"))
}

val detektMain = tasks.named<Detekt>("detektMain")
val detektNegative =
    tasks.register<Detekt>("detektNegative") {
        description = "Verifies that every forbidden Kotlin Java terminal fixture is detected."
        group = LifecycleBasePlugin.VERIFICATION_GROUP
        setSource(layout.projectDirectory.dir("src/detektNegative/kotlin"))
        classpath.setFrom(detektMain.map { it.classpath })
        config.setFrom(detektMain.map { it.config })
        ignoreFailures.set(true)
        reports {
            checkstyle.required.set(true)
            html.required.set(false)
            sarif.required.set(false)
            markdown.required.set(false)
        }
        doLast {
            val report = reports.checkstyle.outputLocation.get().asFile
            val findings = report.useLines { lines -> lines.count { "<error " in it } }
            check(findings == 29) {
                "Expected 29 forbidden Kotlin Java terminal findings, but Detekt reported $findings"
            }
        }
    }

tasks.named("check") { dependsOn(detektNegative) }
