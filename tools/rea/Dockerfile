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
# Pinned: base image by digest (Eclipse Temurin JDK 21 on Ubuntu 24.04, Ghidra 12.1.4
# requires a Java 21 JDK, GettingStarted.md "Minimum Requirements"), Node.js by version and SHA-256 (values from
# https://nodejs.org/dist/v22.23.3/SHASUMS256.txt; rea-agents 6.7.0 declares
# "engines": {"node": "^22.19.0 || ^24.11.0 || >=26.0.0"}), REA by exact version.
# Not pinned: the transitive npm dependencies of rea-agents are resolved by `npm install -g` at build time from
# the version ranges their packages declare (no lockfile is used), and the apt packages of the build stage come
# from the Ubuntu archive of the day. So two builds can differ: the image is not bit-reproducible.
#
# Supply chain: `npm install --ignore-scripts` runs no install script of any package. In rea-agents 6.7.0 the
# only one is that of the optional `frida` package (dynamic instrumentation, not used by the Ghidra lane), which
# is therefore left without its native binding. Two stages: Node.js is downloaded and REA installed in a build
# stage (curl, xz-utils); the final stage only copies /opt/node, and removes the base image's own download tools
# (curl, wget). `procps`, required by REA on Linux (REA docs/installation.md), is part of the base image.
#
# Run time (tools/rea.py): no network (`--network none`), all capabilities dropped, no-new-privileges, and a
# non-root user (the host's uid:gid on Linux/macOS, uid 1000 = the base image's `ubuntu` user on Windows), with
# HOME=/home/rea (world-writable, sticky) so that any uid has a writable home.
#
# Build: py -3 tools/rea.py <game> image   (or: docker build -t gcrt-rea:6.7.0 tools/rea)

ARG BASE=eclipse-temurin:21-jdk-noble@sha256:107215954359f1ab0be058a247d7c64ed6e69c71c1682d471d38ef751ec8e964

FROM ${BASE} AS build

ARG REA_VERSION=6.7.0
ARG NODE_VERSION=22.23.3
ARG NODE_SHA256_X64=df450af89261115ef9f9e3830c3eeb2cc9213b63c720b1af623cb5dcbe2e02de
ARG NODE_SHA256_ARM64=a44aeb94849a299b22df10b9e622ec2f605c2183501bc40590705131de7c740f
ARG TARGETARCH=amd64

RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates xz-utils \
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
    mv "/opt/node-v${NODE_VERSION}-linux-${arch}" /opt/node

ENV PATH=/opt/node/bin:${PATH}
RUN npm install -g --ignore-scripts "rea-agents@${REA_VERSION}" \
 && rea --version

FROM ${BASE}

COPY --from=build /opt/node /opt/node

RUN set -eu; \
    apt-get purge -y --auto-remove curl wget; \
    rm -rf /var/lib/apt/lists/*; \
    for tool in node npm npx rea; do ln -s "/opt/node/bin/${tool}" "/usr/local/bin/${tool}"; done; \
    mkdir -m 1777 /home/rea; \
    command -v ps >/dev/null; \
    rea --version

ENV GHIDRA_INSTALL_DIR=/opt/ghidra \
    HOME=/home/rea
WORKDIR /work
USER 1000:1000
