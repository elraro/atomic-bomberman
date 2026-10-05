# Android build

The Android app is the same game: the C++ code of the repository built with the Android NDK, started by SDL's activity. This folder holds only the wrapping (Gradle files and the manifest).

## What it is like

- Without anything else it plays with the **free asset set** (made by the program itself). With a copy of the original game it plays with the original's data: see below.
- **Touch controls**: a direction pad at the bottom left, two buttons at the bottom right (red: bomb, also Enter in the menus; blue: action, also Space), and two small ones at the top (left: back / Esc, right: chat). The phone's back key is Esc too. On a wide screen the game sits in the middle and the controls beside it.
- A **gamepad** or a **keyboard** connected to the device works as on a desktop.
- **Network games**: joining works as on a desktop. Hosting from the phone works on a local network; from outside it depends on the phone's network letting connections in, which mobile networks do not.
- Landscape only. OpenGL ES 3.0 and Android 7.0 (API 24) or later.

## Using the original game's data

The app looks on every start into one folder of the device:

```
Android/data/io.github.elraro.atomicbomberman/files/original
```

(on the device's shared storage; the first start shows the full path and creates the folder, with a note in it).

1. Start the app once. It says where the folder is and goes on with the free asset set.
2. Copy your copy of the original game into that folder: the installed game or the whole CD, that is the folder holding `COLOR.PAL` and `DATA`. It may be put there directly or as one folder inside. About 550 MB.
3. Start the app again. It converts the data (a screen with a progress bar, the file in hand and the time left; under a minute on the emulator) into its private storage, about 300 MB, and from then on plays with the original graphics, sounds, levels and intro.

Things to know:

- **Each new release converts again**: the converted files carry the release that made them, and a start with another release redoes them. So leave the original files in the folder. If they are gone, the files converted before stay in use.
- The conversion is made beside the files in use and replaces them only when all of it went well. If it fails (storage full) or the app is closed half way, what was there stays, the reason is shown, and the next start tries again.
- If the folder holds files but no game is recognised in it, a start says so.
- **Reaching the folder**: from a computer over USB (file transfer), or with `adb`. On Android 11 and later the file managers on the device itself are mostly not allowed into `Android/data`. With `adb`, pushing straight into that folder may be refused; this works: `adb push GAME /data/local/tmp/ATOMIC`, then `adb shell cp -r /data/local/tmp/ATOMIC /sdcard/Android/data/io.github.elraro.atomicbomberman/files/original/`.
- Uninstalling the app removes the folder and the converted files with it.

The same can be tried on a desktop with `atomic --import-folder DIR` (there `--import-assets` is the usual way).

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

Built and started on the Android emulator (API 34, x86_64): the title, the menus, a local match and the touch controls were seen working. The import of the original data was run there too: first-start note, conversion with its progress screen (260 data files, 971 sounds), original intro and menu afterwards, no second conversion on the next start. Not yet run on a real phone or tablet; sound was not listened to; network play from the app was not tried.
