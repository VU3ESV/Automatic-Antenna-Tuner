#pragma once

// Build identity, shown in the boot banner, /api/status and the browser
// page so an over-Ethernet update can be seen to have landed. The
// definitions live in src/build_info.cpp, which tools/build_info.py
// regenerates before every build (the .cpp is not tracked by git).

extern const char kBuildStamp[];   // "2026-09-06 13:05:12 +0530"
extern const char kBuildGit[];     // "b297b3d" or "b297b3d-dirty"; "nogit" outside a checkout
extern const char kBuildEnv[];     // PlatformIO environment, e.g. "teensy41_native"
