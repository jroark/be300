# Patched Unicorn 2.1.4 (Linux, aarch64)

This directory contains a locally built copy of Unicorn 2.1.4 for Linux
(aarch64). The macOS build lives in `third_party/unicorn/`; this sibling
tree exists so we can build and run the emulator inside the Ubuntu 22.04
container with the same Unicorn version/features.

## Build provenance

* Host: Docker `ubuntu:22.04` (aarch64)
* Source: `https://github.com/unicorn-engine/unicorn.git` tag `2.1.4`
* Configure:

  ```bash
  cmake ../unicorn-src \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX=/work/third_party/unicorn-linux
  ```

* Build + install:

  ```bash
  cmake --build . -j$(nproc)
  cmake --install .
  ```

`pkg-config` will now find this build automatically because the Docker image
and `docker-compose.yml` export `PKG_CONFIG_PATH=/work/third_party/unicorn-linux/lib/pkgconfig`.
