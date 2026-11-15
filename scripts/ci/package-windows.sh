#!/bin/bash
# The Windows packages of build_win, in two independent parts that the workflow runs beside the native tests.
#
# usage (inside an MSYS2 shell, from the repository root, after the build):  package-windows.sh installer | portable
#
#   installer  the Inno Setup engine: install into build/inno_staging, breakpad symbols from the unstripped modules, strip the staged
#              files, compile the installer (build/cpack_artifacts/Sunshine-engine.exe) and write artifacts/sunshine-symbols.zip.
#   portable   the portable ZIP (cpack): artifacts/sunshine-windows-portable.zip.
#
# Why two parts that run beside `ctest`: both only read what the build produced (cmake --install and cpack copy into trees of their
# own and strip those copies; the tests run the build's own binaries), they share no file except the directory cpack_artifacts, and the
# step used to wait for the tests before it started the 30 to 60 s they take together. The parts are started by the workflow step "Run
# native tests and package" and their logs are shown after the test output.
#
# Environment (set by the step): RTX_VIDEO_ADAPTER_BUILT, RTX_VIDEO_ADAPTER_REQUIRED, DUMP_SYMS, SYMBOLS_TAG, SYMBOLS_COMMIT,
# GITHUB_REPOSITORY, GITHUB_WORKSPACE.
set -euo pipefail

part="${1:-}"
mkdir -p artifacts build/cpack_artifacts
cd build

# The NVIDIA DLSS NR adapter is optional: its files are checked when the build produced it.
dlssnr_adapter_built() { [[ -f ./image_enhancement/nvidia_dlssnr_adapter/Release/foundation_dlssnr_adapter.dll ]]; }

installer() {
  if [[ "$RTX_VIDEO_ADAPTER_REQUIRED" == "true" && "$RTX_VIDEO_ADAPTER_BUILT" != "true" ]]; then
    echo "Protected master build did not produce the required NVIDIA RTX Video adapter"
    exit 1
  fi

  # Package the Inno Setup installer. Install into the staging directory first.
  cmake --install . --prefix ./inno_staging

  # Symbols from the unstripped modules, then strip what ships: the packages carry no DWARF, the symbol zip does.
  # The key is the build-id GUID of each module, which stripping and signing keep.
  python3 ../scripts/ci/make-symbols.py --staging ./inno_staging --build . --dump-syms "$(cygpath -u "$DUMP_SYMS")" \
    --output ../artifacts/sunshine-symbols.zip --tag "$SYMBOLS_TAG" --commit "$SYMBOLS_COMMIT" --repository "$GITHUB_REPOSITORY" \
    --source-root "$(cygpath -m "$GITHUB_WORKSPACE")" --source-root "$GITHUB_WORKSPACE" \
    --require sunshine.exe --require sunshinesvc.exe --strip-staged
  if [[ "$RTX_VIDEO_ADAPTER_BUILT" == "true" ]]; then
    test -f ./inno_staging/tools/hdr_enhanced/nvidia_rtx_video/foundation_rtx_video_adapter.dll
    test ! -e ./inno_staging/msvcp140.dll
    test ! -e ./inno_staging/vcruntime140.dll
  fi
  test ! -e ./inno_staging/tools/hdr_enhanced/nvidia_rtx_video/nvngx_truehdr.dll
  test ! -e ./inno_staging/tools/hdr_enhanced/nvidia_dlssnr/nvngx_dlssnr.dll
  if dlssnr_adapter_built; then
    test -f ./inno_staging/tools/hdr_enhanced/nvidia_dlssnr/foundation_dlssnr_adapter.dll
    test -f ./inno_staging/tools/hdr_enhanced/nvidia_dlssnr/NVIDIA-DLSS-LICENSE.txt
    test -f ./inno_staging/tools/hdr_enhanced/nvidia_dlssnr/NVIDIA-OPTICAL-FLOW-NOTICES.txt
  fi
  test ! -e ./inno_staging/assets/hdr-components.json

  # Run the Inno Setup compiler and prove that it consumed only the
  # lightweight package manifest, not the self-contained runtime.
  "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" sunshine_installer.iss | tee inno-build.log
  if grep -Ei 'nvngx_(truehdr|dlssnr)\.dll' inno-build.log; then
    echo "NVIDIA enhancement runtimes must not be compiled into the installer"
    exit 1
  fi
  if dlssnr_adapter_built; then
    grep -F "foundation_dlssnr_adapter.dll" inno-build.log
    grep -F "NVIDIA-DLSS-LICENSE.txt" inno-build.log
    grep -F "NVIDIA-OPTICAL-FLOW-NOTICES.txt" inno-build.log
  fi
  if [[ "$RTX_VIDEO_ADAPTER_BUILT" == "true" ]]; then
    grep -F "foundation_rtx_video_adapter.dll" inno-build.log
  fi

  # The engine is not the download: "Assemble installer" wraps it in the shell.
  test -f ./cpack_artifacts/Sunshine-engine.exe
}

portable() {
  # Generate the portable ZIP.
  cpack -G ZIP --config ./CPackConfig.cmake -D CPACK_STRIP_FILES=ON --verbose
  cmake -E tar tf ./cpack_artifacts/Sunshine.zip | tr '\\' '/' > portable-files.txt
  if [[ "$RTX_VIDEO_ADAPTER_BUILT" == "true" ]]; then
    grep -F "Sunshine/tools/hdr_enhanced/nvidia_rtx_video/foundation_rtx_video_adapter.dll" portable-files.txt
  fi
  if dlssnr_adapter_built; then
    grep -F "Sunshine/tools/hdr_enhanced/nvidia_dlssnr/foundation_dlssnr_adapter.dll" portable-files.txt
    grep -F "Sunshine/tools/hdr_enhanced/nvidia_dlssnr/NVIDIA-DLSS-LICENSE.txt" portable-files.txt
    grep -F "Sunshine/tools/hdr_enhanced/nvidia_dlssnr/NVIDIA-OPTICAL-FLOW-NOTICES.txt" portable-files.txt
  fi
  if grep -Ei 'nvngx_(truehdr|dlssnr)\.dll|hdr-components\.json' portable-files.txt; then
    echo "Side-loaded HDR files and mutable trust metadata must not be included in the main portable package"
    exit 1
  fi
  mv ./cpack_artifacts/Sunshine.zip ../artifacts/sunshine-windows-portable.zip
}

case "$part" in
  installer) installer ;;
  portable) portable ;;
  *) echo "usage: $0 installer|portable" >&2; exit 2 ;;
esac
