#!/bin/sh
# Builds WDVR.asi (64-bit Windows) with llvm-mingw.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++}
CC=${CC:-tools/llvm-mingw/bin/x86_64-w64-mingw32-clang}
mkdir -p build/obj out
INC="-Isrc -Ideps/imgui -Ideps/imgui/backends -Ideps/minhook/include -Ideps/openvr/headers"
FLAGS="-O2 -DNDEBUG -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00"

objs=""
for f in src/mod.cpp src/d3dcompile_shim.cpp src/camera.cpp src/input.cpp src/framecap.cpp \
         deps/imgui/imgui.cpp deps/imgui/imgui_draw.cpp deps/imgui/imgui_tables.cpp \
         deps/imgui/imgui_widgets.cpp deps/imgui/backends/imgui_impl_win32.cpp \
         deps/imgui/backends/imgui_impl_dx11.cpp; do
  o=build/obj/$(basename "$f").o
  $CXX -std=c++20 $FLAGS $INC -c "$f" -o "$o"
  objs="$objs $o"
done
for f in src/vr.c deps/minhook/src/buffer.c deps/minhook/src/hook.c \
         deps/minhook/src/trampoline.c deps/minhook/src/hde/hde64.c; do
  o=build/obj/$(basename "$f").o
  $CC -std=c11 $FLAGS $INC -c "$f" -o "$o"
  objs="$objs $o"
done

# ASI plugin, loaded by Ultimate ASI Loader (dinput8.dll).
$CXX -std=c++20 $FLAGS $INC -c src/asi_main.cpp -o build/obj/asi_main.o
$CXX -shared -static -s -o out/WDVR.asi build/obj/asi_main.o $objs \
  -ldxguid -luser32 -lgdi32 -ldwmapi -limm32
echo "Built out/WDVR.asi"
