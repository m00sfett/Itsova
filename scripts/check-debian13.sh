#!/usr/bin/env bash
# Builds glimply and runs its tests in a Debian 13 (trixie) container, the oldest supported
# toolchain (#55): Qt 6.8, KDE Frameworks 6.13, CMake 3.31. The source is mounted read-only;
# the build happens inside the container. Needs Docker. Usage: scripts/check-debian13.sh
set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="debian:trixie"
packages=(
    cmake ninja-build g++ python3 gettext
    qt6-base-dev qt6-base-dev-tools qt6-image-formats-plugins qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools
    libkf6config-dev libkf6globalaccel-dev libkf6statusnotifieritem-dev libkf6notifications-dev
    libkf6dbusaddons-dev libkf6widgetsaddons-dev libkf6xmlgui-dev libkf6i18n-dev
    qt6-wayland-dev qt6-wayland-dev-tools plasma-wayland-protocols libkpipewire-dev ffmpeg
)

docker run --rm --cpus 4 --memory 8g \
    -v "${source_dir}:/src:ro" \
    -e DEBIAN_FRONTEND=noninteractive -e QT_QPA_PLATFORM=offscreen \
    "${image}" bash -c "
        set -euo pipefail
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends ${packages[*]} >/dev/null
        echo \"== toolchain: Qt \$(dpkg-query -W -f='\${Version}' qt6-base-dev), KF6 \$(dpkg-query -W -f='\${Version}' libkf6config-dev), CMake \$(dpkg-query -W -f='\${Version}' cmake)\"
        cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-Wall -Wextra'
        cmake --build /build -- -k 0
        ctest --test-dir /build --output-on-failure
    "
