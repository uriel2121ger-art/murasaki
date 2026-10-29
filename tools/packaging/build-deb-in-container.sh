#!/usr/bin/env bash
# Build the .deb inside the Ubuntu release it is meant for, so its dependencies (glibc, Qt, FFmpeg pins from
# dpkg-shlibdeps) are the ones of that release. Needs docker usable without sudo. Nothing is installed on the host.
#   tools/packaging/build-deb-in-container.sh [BASE_IMAGE]      default ubuntu:26.04, e.g. ubuntu:25.10; the GUI needs Qt >= 6.8, so not 24.04
# Output: build/deb-<release>/murasaki_<version>_<arch>.deb
set -euo pipefail
base="${1:-ubuntu:26.04}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tag="${base//[:\/]/-}"
out="$root/build/deb-$tag"
fetch="${MURASAKI_FETCH_DIR:-$HOME/.cache/murasaki-tests/fetch}"
mkdir -p "$out" "$fetch"

docker build -q --build-arg "BASE=$base" -t "murasaki-deb-build:$tag" -f "$root/tools/packaging/Dockerfile.deb" "$root/tools/packaging"

# The source is mounted read-only and the build runs as the calling user: the output is not root-owned.
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$root:/src:ro" -v "$out:/out" -v "$fetch:/fetch" "murasaki-deb-build:$tag" bash -euxc '
    cmake -S /src -B /out/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user -DBUILD_TESTING=OFF \
      -DMURASAKI_BUILD_SURFACE=ON -DMURASAKI_BUILD_APP=ON -DMURASAKI_FETCH_DIR=/fetch
    cmake --build /out/b -j"$(nproc)"
    (cd /out/b && cpack -G DEB && cp murasaki_*.deb /out/)
  '
ls -l "$out"/murasaki_*.deb
dpkg-deb -I "$out"/murasaki_*.deb | grep -E "Package|Version|Depends" || true
