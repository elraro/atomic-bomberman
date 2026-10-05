# Android build

The Android app is the same game: the C++ code of the repository built with the Android NDK, started by SDL's activity. This folder holds only the wrapping (Gradle files and the manifest).

## What it is like

- It plays with the **free asset set** (made by the program itself). Importing the data of an original copy is not offered on Android yet.
- **Touch controls**: a direction pad at the bottom left, two buttons at the bottom right (red: bomb, also Enter in the menus; blue: action, also Space), and two small ones at the top (left: back / Esc, right: chat). The phone's back key is Esc too. On a wide screen the game sits in the middle and the controls beside it.
- A **gamepad** or a **keyboard** connected to the device works as on a desktop.
- **Network games**: joining works as on a desktop. Hosting from the phone works on a local network; from outside it depends on the phone's network letting connections in, which mobile networks do not.
- Landscape only. OpenGL ES 3.0 and Android 7.0 (API 24) or later.

## Building it

Needs the Android SDK with platform 35, build tools 35.0.0, NDK 27.2.12479018 and CMake 3.22.1, a JDK 17 or 21, and Gradle 8.9 or later (8.11.1 is what was used).

```sh
android/fetch-sdl.sh                 # SDL's sources, once (into android/SDL)
cd android
gradle assembleDebug                 # app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

`ANDROID_HOME` must point at the SDK and `JAVA_HOME` at the JDK. The GitHub workflow does the same and publishes the APK as an artifact (and with releases).

To try the phone's code paths on a desktop: `atomic --gles --touch` (OpenGL ES, and the on-screen controls worked with the mouse).

## Signing

The APK is signed with the debug key of whichever machine built it. It installs (allow installing from unknown sources), but a build from another machine will not install over it: uninstall the old one first. For an app store, or for updates that install over each other, a release key has to be created and configured in `app/build.gradle`; that key must not be put into the repository.

## State

Built and started on the Android emulator (API 34, x86_64): the title, the menus, a local match and the touch controls were seen working. Not yet run on a real phone or tablet; sound was not listened to; network play from the app was not tried.
