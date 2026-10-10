#!/bin/bash
# S1: static wasm32 link of glgears + Mesa (softpipe + Zink, surfaceless EGL,
# desktop GL via eglGetProcAddress) + the guest Vulkan ICD + Xlib.
#
# usage: build-glgears.sh TASKDIR BUILDNAME OUTNAME
#   TASKDIR/build/BUILDNAME : ninja tree from tools/zink/configure-wasm32.sh TASKDIR BUILDNAME
#                             -Dgallium-drivers=zink,softpipe (objects compiled; the .so
#                             link edges fail by design, wasm-ld has no shared libraries)
#   TASKDIR/icd/src         : default snapshot of web/linux/tests/{guest-vulkan,guest-gpu} from git
#   ICD_SRC=DIR (env)       : use another snapshot dir instead (e.g. `git archive COMMIT` extract);
#                             never point it at the live worktree while others edit it
#   TASKDIR/icd/vk-headers-231 : Vulkan-Headers v1.3.231 include dir used by the ICD
# Outputs in TASKDIR/link/OUTNAME (must not exist): archives, glgears.wasm, logs, sha256.
# The archive list is read from the libgallium-*.so edge in build.ninja, so it
# follows the configured gallium drivers. tools/zink/link-wasm32.sh is unchanged.
set -u -o pipefail
T=$1; NAME=$2; OUTNAME=$3
ICD_SRC=${ICD_SRC:-$T/icd/src}
export TMPDIR=$T/tmp TMP=$T/tmp TEMP=$T/tmp XDG_CACHE_HOME=$T/cache
case "$(stat -f -c %T "$T")" in tmpfs|ramfs) echo "refusing memory filesystem: $T" >&2; exit 1;; esac
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
B=$T/build/$NAME; O=$T/link/$OUTNAME
[ -e "$O" ] && { echo "output exists (never overwrite runs): $O" >&2; exit 1; }
WRAP=/nix/store/j8mdl69qafyijllv7s703219nb5fnrdp-wasm32-unknown-linux-musl-llvm-toolchain-wrapper-22.1.8/bin
CC=$WRAP/wasm32-unknown-linux-musl-clang; CXX=$WRAP/wasm32-unknown-linux-musl-clang++
AR=llvm-ar
SHIMS=$ROOT/web/linux/firefox-shims
X11=/nix/store/gqaysz4gcpwh3hjpj55k8j9mf7lya7xs-libX11-static-wasm32-unknown-linux-musl-1.8.13
PROTO=/nix/store/7fl11iw0s4srx2300pnb28wwpibky6wh-xorgproto-static-wasm32-unknown-linux-musl-2024.1
XCB=/nix/store/wlbxw5w935lipm9imsz9jvsyk0mm322x-libxcb-static-wasm32-unknown-linux-musl-1.17.0
XAU=/nix/store/r79l4jh7kmlcz6vmfiv02gw1p1iqb4m5-libXau-static-wasm32-unknown-linux-musl-1.0.12
MESA=$(sed -n 's/^.*meson setup .* \([^ ]*mesa-[0-9.]*\) .*$/\1/p' "$B/meson-logs/meson-log.txt" | head -1)
[ -d "$MESA" ] || MESA=$T/mesa-26.2.3
mkdir -p "$O/obj" "$O/lib" "$O/x11" "$T/tmp"
log() { echo "== $*" | tee -a "$O/steps.log"; }
run() { local n=$1; shift; printf '%q ' "$@" > "$O/$n.command.txt"; echo >> "$O/$n.command.txt"
        "$@" > "$O/$n.log" 2>&1; local rc=$?; log "$n rc=$rc"; return $rc; }
CFLAGS=(-O2 -pthread -matomics -mbulk-memory -ffunction-sections -fdata-sections)

# 0. frozen inputs: Xlib closure (copied, hashed), app source
cp -a "$X11/include" "$O/x11/x11-include"; cp -a "$PROTO/include" "$O/x11/proto-include"
cp "$X11/lib/libX11.a" "$XCB/lib/libxcb.a" "$XAU/lib/libXau.a" "$O/x11/"
cp "$ROOT/web/linux/tests/glgears/glgears.c" "$O/glgears.c"
echo "ICD_SRC=$ICD_SRC" > "$O/icd-src.txt"
(cd "$O" && sha256sum glgears.c x11/*.a "$ICD_SRC/guest-vulkan/icd.c" "$ICD_SRC/guest-gpu/guest-gpu.c" "$HERE/../zink/shims/zink_static_vk.c") > "$O/inputs-sha256.txt"

# 1. adapters / shims (same as tools/zink/link-wasm32.sh)
run cc-zink-static-vk "$CC" "${CFLAGS[@]}" -std=c11 -Wall -Wextra -Werror -c "$ROOT/tools/zink/shims/zink_static_vk.c" -o "$O/obj/zink_static_vk.o" || exit 1
run cc-mmap-shim "$CC" "${CFLAGS[@]}" -I"$SHIMS" -c "$SHIMS/mmap-shim.c" -o "$O/obj/mmap-shim.o" || exit 1
run cc-unwind-stubs "$CC" "${CFLAGS[@]}" -c "$SHIMS/unwind-stubs.c" -o "$O/obj/unwind-stubs.o" || exit 1
# 2. guest Vulkan ICD (B2a Zink profile) + vsock transport, so the same binary can run on Zink
ICDF=(-std=c11 -O2 -Wall -Wextra -Werror -pthread -matomics -mbulk-memory -ffunction-sections -fdata-sections -I"$T/icd/vk-headers-231")
run cc-icd "$CC" "${ICDF[@]}" -DGUEST_VK_ZINK=1 -c "$ICD_SRC/guest-vulkan/icd.c" -o "$O/obj/icd.o" || exit 1
run cc-transport "$CC" "${ICDF[@]}" -c "$ICD_SRC/guest-gpu/guest-gpu.c" -o "$O/obj/transport.o" || exit 1
# 3. app
run cc-glgears "$CC" "${CFLAGS[@]}" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I"$MESA/include" \
  -I"$O/x11/x11-include" -I"$O/x11/proto-include" -c "$O/glgears.c" -o "$O/obj/glgears.o" || exit 1
# 3b. Mesa override objects (tools/zink/patches/*.patch, see tools/zink/mesa-overrides.sh):
#     B2c fixes the Itanium-ABI member-function-pointer trap in GLSL builtins.
. "$ROOT/tools/zink/mesa-overrides.sh"; mesa_overrides "$MESA" "$B" "$O" || exit 1

# 4. static archives: exactly the archive inputs of the failed libgallium .so edge
cd "$B" || exit 1
EDGE=$(grep -n '^build src/gallium/targets/dri/libgallium-[0-9.]*\.so: ' build.ninja | head -1 | cut -d: -f1)
[ -n "$EDGE" ] || { log "no libgallium edge in build.ninja"; exit 1; }
sed -n "${EDGE},$((EDGE + 4))p" build.ninja | tr ' ' '\n' | grep -E '^(src|subprojects)/.*\.a$' | sort -u > "$O/gallium-archives.txt"
mapfile -t GALLIUM_ARCHIVES < "$O/gallium-archives.txt"
for a in "${GALLIUM_ARCHIVES[@]}"; do [ -f "$a" ] || { log "missing $a (run ninja first)"; exit 1; }; done
log "gallium archives: ${#GALLIUM_ARCHIVES[@]}"
DRI_TARGET=$(ls src/gallium/targets/dri/libgallium-*.so.p/dri_target.c.o)
run ar-gallium "$AR" qcsL "$O/lib/libgallium_s1.a" "$DRI_TARGET" "${GALLIUM_ARCHIVES[@]}" || exit 1
run ar-egl "$AR" qcs "$O/lib/libEGL_mesa.a" src/egl/libEGL.so.1.0.0.p/*.c.o || exit 1
run ar-icd "$AR" qcs "$O/lib/libguest_vk_icd.a" "$O/obj/icd.o" "$O/obj/transport.o" || exit 1
cd "$O" || exit 1

# 5. executable
LINKF=(-pthread -matomics -mbulk-memory -static -nostdlib++
  -Wl,--gc-sections,--import-memory,--export-table,--shared-memory
  -Wl,--initial-memory=16777216,--max-memory=4294967296,-z,stack-size=1048576
  -Wl,--error-limit=0)
OBJS=("$O/obj/glgears.o" "$O/obj/zink_static_vk.o" "$O/obj/mmap-shim.o" "$O/obj/unwind-stubs.o" "${OVR[@]}")
LIBS=("$O/lib/libEGL_mesa.a" "$O/lib/libgallium_s1.a" "$O/lib/libguest_vk_icd.a"
  "$O/x11/libX11.a" "$O/x11/libxcb.a" "$O/x11/libXau.a" -lc++ -lc++abi -lm)
run link "$CXX" "${LINKF[@]}" "${OBJS[@]}" "${LIBS[@]}" -Wl,-Map="$O/link.map" -o "$O/glgears.wasm"
rc=$?
if [ $rc -ne 0 ]; then
  grep -o 'undefined symbol: [^ ]*' "$O/link.log" | sort | uniq -c | sort -rn > "$O/undefined-symbols.txt"
  log "undefined symbols: $(wc -l < "$O/undefined-symbols.txt")"
fi
for f in lib/*.a glgears.wasm; do [ -f "$f" ] && printf '%s %s\n' "$(stat -c %s "$f")" "$(sha256sum "$f")"; done > "$O/artifacts.txt"
cat "$O/artifacts.txt"
if [ -f glgears.wasm ]; then
  run validate wasm-validate --enable-all glgears.wasm || rc=1
  run abi wasm-objdump -x -j Import glgears.wasm
  grep ' <- ' abi.log | sed 's/.* <- //' | sort -u > imports.txt; log "imports: $(tr '\n' ' ' < imports.txt)"
  for s in softpipe_create_screen zink_create_screen dri2_initialize_surfaceless XPutImage vk_icdGetInstanceProcAddr; do
    grep -q "$s" link.map && log "link.map has $s" || { log "link.map MISSING $s"; rc=1; }
  done
fi
exit $rc
