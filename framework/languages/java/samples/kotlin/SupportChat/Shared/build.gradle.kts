plugins {
    id("org.jetbrains.kotlin.jvm")
}

dependencies {
    api(zlinkLibs.zlink.framework.core)
    api(zlinkLibs.zlink.framework.kotlin)
    api("com.fasterxml.jackson.module:jackson-module-kotlin:2.17.2")
}
