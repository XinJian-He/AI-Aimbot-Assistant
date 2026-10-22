# Third-Party Notices

`aimlib` is distributed under the **GNU General Public License v3.0** (see
[LICENSE](LICENSE)). This file lists the third-party components used by the
project and the licenses that apply to them.

## Bundled source code

| Component | Version | License | Location |
|-----------|---------|---------|----------|
| Dear ImGui | 1.92.9b | MIT | `third_party/imgui/` |
| makcu-cpp | commit `9874efa` | GPL-3.0 | `third_party/makcu-cpp/` |

`makcu-cpp` is compiled directly into `aimlib.dll`. Because GPL-3.0 is a
copyleft license, the combined `aimlib` binary is distributed under GPL-3.0 as
well. The original license texts are kept in the component directories:

- `third_party/imgui/LICENSE.txt`
- `third_party/makcu-cpp/LICENSE`

## Prebuilt dependencies (not distributed in this repository)

These SDKs are downloaded separately into `deps/` and are excluded from version
control (see [deps/README.md](deps/README.md)). They are not part of the project
source and remain under their own licenses.

| Component | Version | License |
|-----------|---------|---------|
| OpenCV | 4.11.0 | Apache License 2.0 |
| ONNX Runtime | 1.24.x (DirectML build) | MIT |
| DirectML | 1.15.4 (bundled with ONNX Runtime) | Microsoft DirectML license |

When redistributing a binary that includes these libraries, review and comply
with the corresponding upstream license and redistribution terms.

## Embedded binary modules

The following binary modules are packed into `aimlib.dll` at build time (their
encrypted byte arrays live in `src/embed/*_data.h`):

- `embedded/封装dll_unpacked.dll`
- `embedded/NeuralCurveModule_unpacked.dll`

These modules are separately authored components. They are **not** covered by
the GPL-3.0 grant that applies to the project's own source code; the terms of
their respective rights holders apply. If you redistribute this repository or a
built binary, make sure you have the right to redistribute these modules. If you
do not, remove them and provide equivalent modules through the runtime fallback
(`封装dll.dll` / `NeuralCurveModule.dll` next to the host executable).

## Historical third-party code

The previous `third_party/mak-suite` reference tree is not part of the current
build and has been removed from the repository layout.
