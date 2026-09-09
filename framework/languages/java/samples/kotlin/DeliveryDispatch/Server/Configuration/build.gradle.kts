plugins {
    `java-library`
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api(project("${path.substringBefore(":Server")}:Shared"))
    api(zlinkLibs.zlink.framework.core)
    api(zlinkLibs.zlink.framework.locations.redis)
}
