# macOS packaging

- `Info.plist.in`: the application bundle's description, filled in by CMake (`-DAB_MACOS_APP=ON`).
- `icon-1024.png`: the app's icon. It is this project's own picture: the free asset set's menu pointer (the robot's head, `src/free/free_art.cpp`) in the yellow player colour, enlarged 18 times without smoothing, on a dark blue rounded square. The release build turns it into an `.icns` file with `sips` and `iconutil` (see `.github/workflows/build.yml`).

The bundle is built and signed ad hoc by the `macos` job of the workflow. It is not signed with an Apple developer certificate and not notarized; `INSTALL.md` says what that means for the person opening it.
