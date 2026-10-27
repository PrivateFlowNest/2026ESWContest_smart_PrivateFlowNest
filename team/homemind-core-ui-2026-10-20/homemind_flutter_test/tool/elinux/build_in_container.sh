#!/usr/bin/env bash
# Runs inside homemind-ui-elinux-builder (tool/elinux/Dockerfile). PC simulation only.
#
# Mounts:  /src/app  (read-only)  this Flutter app
#          /w/contracts (read-only) <repo>/contracts (used by the app tests)
#          /cache    (volume)     flutter-elinux checkouts, the Flutter SDK they pin, pub cache
#          /out      (bind)       native/.deps/ui-elinux/out/<label>
# Env:     ELINUX_REPO, ELINUX_REF  flutter-elinux git repository and commit
#          PUB_OVERRIDES            "pkg:version ..." written as dependency_overrides in the
#                                   work copy only (the repo pubspec is never changed)
#          ARCHES                   "x64 arm64" (default)
#          DEFINES_x64, DEFINES_arm64  --dart-define values, space separated KEY=VALUE
#          RUN_TESTS=1              also run analyze + the app tests on the pinned Flutter SDK
#          INSTALL_RPATH            runner RUNPATH instead of the template's $ORIGIN/lib
set -euo pipefail

: "${ELINUX_REPO:?}" "${ELINUX_REF:?}"
ARCHES="${ARCHES:-x64 arm64}"
SDK="/cache/flutter-elinux-${ELINUX_REF:0:12}"
export PUB_CACHE=/cache/pub-cache
export PATH="$SDK/bin:$PATH"
export FLUTTER_SUPPRESS_ANALYTICS=true CI=true

log() { printf '\n== %s\n' "$*"; }

if [[ ! -d "$SDK/.git" ]]; then
  log "clone $ELINUX_REPO @ $ELINUX_REF"
  git init -q "$SDK"
  git -C "$SDK" remote add origin "$ELINUX_REPO"
fi
git -C "$SDK" fetch -q --depth=1 origin "$ELINUX_REF"
git -C "$SDK" checkout -q --force FETCH_HEAD
test "$(git -C "$SDK" rev-parse HEAD)" = "$ELINUX_REF"

mkdir -p /out
{
  echo "flutter-elinux repo: $ELINUX_REPO"
  echo "flutter-elinux commit: $(git -C "$SDK" rev-parse HEAD)"
  echo "pinned flutter: $(tr '\n' ' ' < "$SDK/bin/internal/flutter.version")"
  echo "pinned elinux engine: $(cat "$SDK/bin/internal/engine.version")"
} | tee /out/versions.txt

log "flutter-elinux setup (clones the pinned Flutter SDK on first run)"
flutter-elinux precache > /out/setup.log 2>&1 || { tail -n 30 /out/setup.log; exit 10; }
{
  flutter-elinux --version 2>&1
  echo "clang: $(clang --version | head -n 1)"
  echo "cmake: $(cmake --version | head -n 1)"
  echo "host: $(uname -m) $(. /etc/os-release && echo "$PRETTY_NAME")"
} | tee -a /out/versions.txt

log "work copy of the app"
# Same depth as in the repo, so the tests find ../../../contracts (mounted read-only at /w/contracts).
work=/w/team/ui/homemind_flutter_test
rm -rf "$work"
mkdir -p "$work"
rsync -a --exclude build --exclude .dart_tool --exclude windows --exclude web \
  --exclude linux --exclude atlas --exclude tool /src/app/ "$work/"
cd "$work"

if [[ -n "${PUB_OVERRIDES:-}" ]]; then
  log "work-copy dependency_overrides: $PUB_OVERRIDES"
  printf '\ndependency_overrides:\n' >> pubspec.yaml
  for item in $PUB_OVERRIDES; do
    printf '  %s: %s\n' "${item%%:*}" "${item#*:}" >> pubspec.yaml
  done
  echo "dependency_overrides: $PUB_OVERRIDES" >> /out/versions.txt
fi

log "flutter-elinux create --platforms=elinux (template runner, same as atlas/runner)"
flutter-elinux create --no-pub --platforms=elinux --org com.homemind \
  --project-name homemind_flutter_test . > /out/create.log 2>&1 || { cat /out/create.log; exit 11; }
tail -n 2 /out/create.log

if [[ -n "${INSTALL_RPATH:-}" ]]; then
  # Atlas starts apps in secure-execution mode: ld.so ignores LD_LIBRARY_PATH and $ORIGIN, so the
  # bundle libraries need an absolute RUNPATH (real Pi5-01, 2026-10-23: "libflutter_engine.so:
  # cannot open shared object file"; the Atlas home app has /usr/atlas/apps/<id>/lib).
  log "runner RUNPATH: $INSTALL_RPATH"
  grep -q 'set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")' elinux/CMakeLists.txt \
    || { echo "template RPATH line not found in elinux/CMakeLists.txt"; exit 12; }
  sed -i "s#set(CMAKE_INSTALL_RPATH \"\$ORIGIN/lib\")#set(CMAKE_INSTALL_RPATH \"$INSTALL_RPATH\")#" \
    elinux/CMakeLists.txt
  echo "runner RUNPATH: $INSTALL_RPATH" >> /out/versions.txt
fi

log "pub get"
set +e
flutter-elinux pub get 2>&1 | tee /out/pub-get.log
status=${PIPESTATUS[0]}
set -e
if [[ $status -ne 0 ]]; then
  echo "PUB_GET_FAILED status=$status" | tee -a /out/versions.txt
  exit 20
fi
awk '/^  [a-z_]+:$/ {name = $1} /^    version:/ {print name, $2}' pubspec.lock \
  | grep -E '^(mqtt_client|meta|vector_math|material_color_utilities|characters|collection):' \
  | tee /out/resolved-packages.txt

if [[ "${RUN_TESTS:-0}" == 1 ]]; then
  log "analyze + test on the pinned SDK (flutter-tester, Linux x64)"
  status=0
  flutter-elinux analyze > /out/analyze.log 2>&1 || status=$?
  echo "analyze status=$status" | tee -a /out/versions.txt
  tail -n 3 /out/analyze.log
  status=0
  flutter-elinux test -r expanded > /out/test.log 2>&1 || status=$?
  echo "test status=$status" | tee -a /out/versions.txt
  tail -n 2 /out/test.log
fi

for arch in $ARCHES; do
  defines_var="DEFINES_${arch}"
  defines=()
  for kv in ${!defines_var:-}; do defines+=("--dart-define=$kv"); done
  cross=()
  if [[ "$arch" == arm64 ]]; then
    cross=(--target-compiler-triple=aarch64-linux-gnu --target-sysroot=/)
  fi
  log "build elinux release $arch wayland ${defines[*]:-}"
  set +e
  flutter-elinux build elinux --release --target-arch="$arch" --target-backend-type=wayland \
    "${cross[@]}" "${defines[@]}" 2>&1 | tee "/out/build-$arch.log"
  status=${PIPESTATUS[0]}
  set -e
  if [[ $status -ne 0 ]]; then
    echo "BUILD_FAILED arch=$arch status=$status" | tee -a /out/versions.txt
    exit 21
  fi

  bundle="build/elinux/$arch/release/bundle"
  rm -rf "/out/$arch"
  mkdir -p "/out/$arch"
  cp -a "$bundle" "/out/$arch/bundle"
  # Tar from the container file system: file modes survive (the /out bind mount may drop them).
  tar -C "$(dirname "$bundle")" --owner=0 --group=0 --numeric-owner --sort=name \
    --mtime='2026-01-01 00:00Z' -cf - bundle | gzip -n -9 > "/out/homemind-ui-elinux-wayland-$arch.tar.gz"
  tar -tvzf "/out/homemind-ui-elinux-wayland-$arch.tar.gz" > "/out/$arch/tar-listing.txt"
  (
    cd "/out/$arch/bundle"
    find . -type f -print0 | sort -z | xargs -0 sha256sum > "/out/$arch/bundle.sha256"
    file homemind_flutter_test lib/*.so > "/out/$arch/file.txt"
    for f in homemind_flutter_test lib/*.so; do
      echo "## $f"
      readelf -d "$f" | grep -E 'NEEDED|RPATH|RUNPATH' || true
    done > "/out/$arch/needed.txt"
    if [[ "$arch" == x64 ]]; then
      ldd homemind_flutter_test > "/out/$arch/ldd.txt" 2>&1 || true
    fi
  )
done

(cd /out && sha256sum ./*.tar.gz > SHA256SUMS && cat SHA256SUMS)
echo "BUILD_OK"
