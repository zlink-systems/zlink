plugins { kotlin("jvm") version "2.3.21" apply false }

extra["frameworkVersion"] = "0.30.1"

extra["bindingVersion"] = "1.19.1"

subprojects {
    apply(plugin = "org.jetbrains.kotlin.jvm")
    extensions.configure<org.jetbrains.kotlin.gradle.dsl.KotlinJvmProjectExtension> {
        jvmToolchain(25)
    }
    dependencies {
        "implementation"(
            "systems.zlink:zlink-framework-core:${rootProject.extra["frameworkVersion"]}"
        )
        "implementation"(
            "systems.zlink:zlink-framework-kotlin:${rootProject.extra["frameworkVersion"]}"
        )
        "implementation"("systems.zlink:zlink:${rootProject.extra["bindingVersion"]}")
        "implementation"("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
        "implementation"("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    }
}
