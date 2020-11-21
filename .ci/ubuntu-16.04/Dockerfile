FROM ubuntu:16.04

RUN apt update -y \
    && apt upgrade -y

RUN apt install -y \
      wget \
      software-properties-common \
      apt-transport-https ca-certificates

RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - \
    && apt-add-repository "deb http://apt.llvm.org/xenial/ llvm-toolchain-xenial-11 main" \
    && apt update -y

RUN apt install -y \
  clang-11 \
  libc++-11-dev \
  libc++abi-11-dev \
  cmake \
  git \
  libssl-dev \
  make \
 && apt autoclean

ENV CXX=/usr/bin/clang++-11
ENV CXXFLAGS=-stdlib=libc++
