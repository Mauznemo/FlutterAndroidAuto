group = "io.github.mauznemo.android_auto"
version = "1.0"

buildscript {
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
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

android {
    namespace = "io.github.mauznemo.android_auto"

    compileSdk = 36
    // Flutter's own default, and the one tools/build-android-deps.sh builds against.
    ndkVersion = "28.2.13676358"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        // Android 9. AAudio, AImageReader and importing a hardware buffer into EGL are
        // all 26, but telling Android which stream is navigation and which is music,
        // and asking for the voice recognition microphone, is 28.
        minSdk = 28

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
        ndk {
            // The ABIs tools/build-android-deps.sh builds for. armeabi-v7a because the
            // Android car stereos this is for are often 32 bit, x86_64 for the emulator.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.31.6"
        }
    }
}
