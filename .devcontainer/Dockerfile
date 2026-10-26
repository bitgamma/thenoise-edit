# SPDX-License-Identifier: Apache-2.0
FROM mcr.microsoft.com/devcontainers/base:ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG EMSDK_VERSION=6.0.10
ENV EMSDK=/opt/emsdk \
    EM_CONFIG=/opt/emsdk/.emscripten \
    EM_CACHE=/home/vscode/.cache/emscripten \
    PATH=/opt/emsdk/upstream/emscripten:/opt/emsdk/upstream/bin:${PATH}
ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    clangd clang-format gdb \
    python3 python3-numpy python3-pil \
    libpng-dev libjpeg-dev libwebp-dev zlib1g-dev libcurl4-openssl-dev \
    libgl1-mesa-dev libglx-dev libgl1-mesa-dri mesa-utils \
    libx11-dev libxcursor-dev libxi-dev libxtst-dev libgtk-3-dev \
    xvfb xauth zenity \
    fonts-liberation fonts-noto-color-emoji \
    libasound2t64 libatk1.0-0t64 libatk-bridge2.0-0t64 libatspi2.0-0t64 \
    libcups2t64 libdbus-1-3 libdrm2 libgbm1 libglib2.0-0t64 libgtk-3-0t64 \
    libnspr4 libnss3 libxcomposite1 libxdamage1 libxfixes3 \
    libxkbcommon0 libxrandr2 libpango-1.0-0 libcairo2 libwayland-client0 \
    && rm -rf /var/lib/apt/lists/*

# Pin the toolchain so the agent tests a repeatable WASM target.
# Ports (libpng, libjpeg, libwebp) are fetched into EM_CACHE by the first web build.
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git /opt/emsdk \
    && cd /opt/emsdk \
    && ./emsdk install "${EMSDK_VERSION}" \
    && ./emsdk activate "${EMSDK_VERSION}" \
    && install -d -o vscode -g vscode /home/vscode/.cache \
    && install -d -o vscode -g vscode /home/vscode/.cache/emscripten
