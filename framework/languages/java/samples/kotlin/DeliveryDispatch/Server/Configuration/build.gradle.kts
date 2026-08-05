plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api(project("${path.substringBefore(":Server")}:Shared"))
    api("systems.zlink:zlink-framework-core:0.1.0-SNAPSHOT")
    api("systems.zlink:zlink-framework-locations-redis:0.1.0-SNAPSHOT")
}

kotlin {
    jvmToolchain(22)
}
