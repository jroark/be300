FROM ubuntu:22.04

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

# Install base build tools and kernel dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    curl \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    libncurses-dev \
    make \
    gcc \
    libc6-dev \
    xz-utils \
    zstd \
    zlib1g-dev \
    cpio \
    pkg-config \
    python3 \
    python3-pip \
    rsync \
    unzip \
    vim \
    cmake \
    ninja-build \
    meson \
    clang \
    lld \
    gdb \
    libglib2.0-dev \
    libpixman-1-dev \
    libfdt-dev \
    && rm -rf /var/lib/apt/lists/*

# Install MIPS and MIPS64 cross-compilers (little-endian)
# Note: For MIPS64EL, the suffix 'abi64' is required in Ubuntu repositories.
RUN apt-get update && apt-get install -y \
    gcc-mipsel-linux-gnu \
    g++-mipsel-linux-gnu \
    binutils-mipsel-linux-gnu \
    libc6-dev-mipsel-cross \
    gcc-mips64el-linux-gnuabi64 \
    g++-mips64el-linux-gnuabi64 \
    binutils-mips64el-linux-gnuabi64 \
    libc6-dev-mips64el-cross \
    && rm -rf /var/lib/apt/lists/*

# Set up work directory
WORKDIR /work

# Default pkg-config search path for the locally built Unicorn
ENV PKG_CONFIG_PATH=/work/third_party/unicorn-linux/lib/pkgconfig

# Default to bash
CMD ["/bin/bash"]
