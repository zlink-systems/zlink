plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api(project("${path.substringBefore(":Server")}:Shared"))
    api("systems.zlink:zlink-framework-core:0.10.0")
    api("systems.zlink:zlink-framework-locations-redis:0.10.0")
}

kotlin {
    jvmToolchain(22)
}
