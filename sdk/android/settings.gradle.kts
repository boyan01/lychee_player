pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        providers.gradleProperty("lychee.example.repo").orNull?.let {
            maven { url = uri(it) }
        }
        google()
        mavenCentral()
    }
}

rootProject.name = "lychee-android"
include(":example")
if (!providers.gradleProperty("lychee.example.repo").isPresent) {
    include(":lychee", ":ffmpeg")
}
