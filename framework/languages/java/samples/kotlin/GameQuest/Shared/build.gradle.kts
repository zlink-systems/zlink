plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api("com.fasterxml.jackson.core:jackson-annotations:2.17.2")
    api(zlinkLibs.zlink.framework.core)
}
