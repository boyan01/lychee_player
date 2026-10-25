group = "dev.lychee.lychee_player"
version = "1.0-SNAPSHOT"

buildscript {
    val kotlinVersion = "2.4.0"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

plugins {
    id("com.android.library")
}

// The core and its FFmpeg (tool/build_ffmpeg.sh android-ABI) come from the
// repository; only the ABIs FFmpeg was built for are built.
val ffmpegDir = file("../../third_party/ffmpeg")
val ffmpegAbis = listOf("arm64-v8a", "x86_64").filter { file("$ffmpegDir/android-$it").isDirectory }
if (ffmpegAbis.isEmpty()) {
    logger.warn("lychee_player: no Android FFmpeg in $ffmpegDir; run tool/build_ffmpeg.sh android-arm64-v8a")
}

android {
    namespace = "dev.lychee.lychee_player"

    compileSdk = 36

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
        }
    }

    defaultConfig {
        minSdk = 26
        ndk {
            abiFilters += ffmpegAbis
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}
