#!/usr/bin/env bash
# Interpreter-only proof of the RMW sub-instruction tick split (Axis 2).
# Links interp_rmw_test.c against the (main-tree) runtime and runs it. A stub
# gb_dispatch stands in for the generated dispatcher (the test drives the
# interpreter directly).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO=/f/Projects/gbcrecomp/gb-recompiled
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"
printf '#include "gbrt.h"\nvoid gb_dispatch(GBContext* c, uint16_t a){ (void)c; (void)a; }\n' > "$HERE/stub_dispatch.c"
PATH="$CLEAN_PATH" gcc -std=c11 -I"$REPO/runtime/include" \
  "$HERE/interp_rmw_test.c" "$HERE/stub_dispatch.c" \
  "$REPO/build/lib/libgbrt.a" /c/msys64/mingw64/lib/libSDL2.dll.a \
  -lGLESv2 -lEGL -lws2_32 -lcomdlg32 -lshell32 -lole32 -loleaut32 -luuid \
  -ladvapi32 -lgdi32 -lwinmm -lsetupapi -limm32 -lversion -lcurl -lstdc++ -lwinpthread \
  -o "$HERE/interp_rmw_test.exe"
"$HERE/interp_rmw_test.exe"
