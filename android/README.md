# Android build

The Android app is the same game: the C++ code of the repository built with the Android NDK, started by SDL's activity. This folder holds the wrapping: Gradle files, the manifest, and one small Java class (`GameActivity`, SDL's activity plus the folder chooser for the original game's data).

## What it is like

- Without anything else it plays with the **free asset set** (made by the program itself). With a copy of the original game it plays with the original's data: see below.
- **Touch controls**: a direction pad at the bottom left, two buttons at the bottom right (red: bomb, also Enter in the menus; blue: action, also Space), and two small ones at the top (left: back / Esc, right: chat). The phone's back key is Esc too. On a wide screen the game sits in the middle and the controls beside it.
- A **gamepad** or a **keyboard** connected to the device works as on a desktop.
- **Network games**: joining works as on a desktop. Hosting from the phone works on a local network; from outside it depends on the phone's network letting connections in, which mobile networks do not.
- Landscape only. OpenGL ES 3.0 and Android 7.0 (API 24) or later.

## Using the original game's data

**Choosing the folder (the usual way).** Put your copy of the original game somewhere on the device: the installed game or the whole CD, that is the folder holding `COLOR.PAL` and `DATA` (about 550 MB; for instance `Download/ATOMIC`).

1. On its first start the app offers **CHOOSE FOLDER** / **NOT NOW**. Later the same is under **Options, Original Game Data**.
2. Android's own folder chooser opens: go to the game's folder (or the one above it), *Use this folder*, *Allow*.
3. The app reads the files it needs from there, converts them into its private storage (about 300 MB) and plays with the original graphics, sounds, levels and intro. A screen shows the step (1 of 3: reading the files; 2: graphics, levels and schemes; 3: sounds), a progress bar, the file in hand and the time left. On the emulator the whole took about two and a half minutes.

Chosen from Options, the folder is converted at the next start of the app.

Things to know:

- **The choice is remembered**, together with Android's permission to read that folder. **Each new release converts again** by itself (the converted files carry the release that made them), so leave the original files where they are. If they are gone or the permission was withdrawn, the start says so and the files converted before stay in use.
- Android does not let a folder's files be read by path, so they are first copied into `Android/data/io.github.elraro.atomicbomberman/files/import-staging` (about 300 MB more, removed after the conversion). Only what the game uses is fetched.
- Android does not allow choosing the top of the storage or the `Download` folder itself: a folder inside them is fine.
- The conversion is made beside the files in use and replaces them only when all of it went well. If it fails (storage full) or the app is closed half way, what was there stays and the reason is shown.
- Uninstalling the app removes the converted files and forgets the folder.

**Without the chooser.** The app also looks on every start into `Android/data/io.github.elraro.atomicbomberman/files/original` (it creates the folder, with a note in it). A copy of the game put there (directly or one folder below) is converted the same way and wins over a chosen folder. That folder can be reached from a computer over USB or with `adb`; pushing straight into it may be refused, this works: `adb push GAME /data/local/tmp/ATOMIC`, then `adb shell cp -r /data/local/tmp/ATOMIC /sdcard/Android/data/io.github.elraro.atomicbomberman/files/original/`.

On a desktop the folder way can be tried with `atomic --import-folder DIR` (there `--import-assets` is the usual way).

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

Built and started on the Android emulator (API 34, x86_64): the title, the menus, a local match and the touch controls were seen working. The import of the original data was run there too, both ways (the folder chooser from the first start and from Options; a copy pushed into the app's folder): first-start note, conversion with its progress screen (260 data files, 971 sounds), original intro and menu afterwards, no second conversion on the next start. Not yet run on a real phone or tablet; sound was not listened to; network play from the app was not tried.
