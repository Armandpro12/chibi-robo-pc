# Linux image for REA (Reverse Engineer Anything, npm package `rea-agents`, MIT License,
# https://github.com/morluto/rea), used by tools/rea.py.
#
# Why a container: REA's PowerPC lane ("PowerPC ELF through Ghidra", REA docs/ghidra-ppc.md) runs on
# Linux/macOS hosts only; on Windows REA admits PE x86/x64/ARM targets only (docs/windows-ghidra-p0.md).
#
# Ghidra is NOT in the image: tools/rea.py mounts the host's Ghidra installation read-only at /opt/ghidra
# (the Java distribution is platform-independent and ships the Linux decompiler binaries), so the language
# extensions installed on the host (e.g. a Gekko/Broadway language) are the ones REA sees.
#
# Everything is pinned: base image by digest (Eclipse Temurin JDK 21 on Ubuntu 24.04, Ghidra 12.1.4
# requires a Java 21 JDK, GettingStarted.md "Minimum Requirements"), Node.js by version and SHA-256 (values from https://nodejs.org/dist/v22.23.3/SHASUMS256.txt;
# rea-agents 6.7.0 declares "engines": {"node": "^22.19.0 || ^24.11.0 || >=26.0.0"}), REA by exact version.
# `procps` is required by REA on Linux (REA docs/installation.md).
#
# Build: py -3 tools/rea.py <game> image   (or: docker build -t gcrt-rea:6.7.0 tools/rea)

FROM eclipse-temurin:21-jdk-noble@sha256:107215954359f1ab0be058a247d7c64ed6e69c71c1682d471d38ef751ec8e964

ARG REA_VERSION=6.7.0
ARG NODE_VERSION=22.23.3
ARG NODE_SHA256_X64=df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de
ARG NODE_SHA256_ARM64=a44aeb94849a299b22df10b9e622ec2f605c2183501bc40590705131de7c740f
ARG TARGETARCH=amd64

RUN apt-get update \
 && apt-get install -y --no-install-recommends procps curl ca-certificates xz-utils \
 && rm -rf /var/lib/apt/lists/*

RUN set -eu; \
    case "${TARGETARCH:-amd64}" in \
      amd64) arch=x64; sum="${NODE_SHA256_X64}" ;; \
      arm64) arch=arm64; sum="${NODE_SHA256_ARM64}" ;; \
      *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    tarball="node-v${NODE_VERSION}-linux-${arch}.tar.xz"; \
    curl -fsSLo "/tmp/${tarball}" "https://nodejs.org/dist/v${NODE_VERSION}/${tarball}"; \
    echo "${sum}  /tmp/${tarball}" | sha256sum -c -; \
    tar -xJf "/tmp/${tarball}" -C /opt; \
    rm "/tmp/${tarball}"; \
    ln -s "/opt/node-v${NODE_VERSION}-linux-${arch}" /opt/node; \
    for tool in node npm npx; do ln -s "/opt/node/bin/${tool}" "/usr/local/bin/${tool}"; done

RUN npm install -g "rea-agents@${REA_VERSION}" \
 && ln -sf /opt/node/bin/rea /usr/local/bin/rea \
 && rea --version

ENV GHIDRA_INSTALL_DIR=/opt/ghidra
WORKDIR /work
