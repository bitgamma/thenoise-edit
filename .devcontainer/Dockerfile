# SPDX-License-Identifier: Apache-2.0
FROM mcr.microsoft.com/devcontainers/base:ubuntu24.04

ARG DEBIAN_FRONTEND=noninteractive
ENV LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    clangd clang-format gdb \
    python3 python3-numpy python3-pil \
    libpng-dev libjpeg-dev libwebp-dev zlib1g-dev libcurl4-openssl-dev \
    libgl1-mesa-dev libglx-dev libgl1-mesa-dri mesa-utils \
    libx11-dev libxcursor-dev libxi-dev libxtst-dev \
    xvfb xauth zenity \
    && rm -rf /var/lib/apt/lists/*
