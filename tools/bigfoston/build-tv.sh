#!/bin/sh
# build-tv.sh — compila o player da TV (bftvd) para o FOSTON: ffmpeg 6.1.5 mínimo (H.264, AAC
# LATM, MPEG-TS; ARMv6 sem ARMv6T2/VFP/NEON) e fdk-aac 2.0.3 (ponto fixo), estáticos, e liga o
# bftvd (src/tv) com o g++ do toolchain C++ (a fdk-aac é C++).
#
#   tools/bigfoston/build-tv.sh            -> firmware/build/ffmpeg-tv/bftvd (e bftv)
#
# Precisa dos dois toolchains do Buildroot já compilados: firmware/build/out (C) e
# firmware/build/out-cxx (com C++), e dos pacotes-fonte em firmware/build/dl/{ffmpeg,fdk-aac}
# (o Buildroot baixa ao compilar com esses pacotes; ou baixe à mão de ffmpeg.org e do
# github.com/mstorsjo/fdk-aac). O bftvd vai para o cartão: BigFoston/tv/bftvd (docs/tv).
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$R/firmware/build
W=$B/ffmpeg-tv
I=$W/inst
C=$B/out/host/bin/arm-buildroot-linux-musleabi-
X=$B/out-cxx/host/bin/arm-buildroot-linux-musleabi-
J=$(nproc 2>/dev/null || echo 4)
[ -x "${C}gcc" ] || { echo "falta o toolchain C: $B/out (make no Buildroot)"; exit 1; }
[ -x "${X}g++" ] || { echo "falta o toolchain C++: $B/out-cxx"; exit 1; }
mkdir -p "$W"

if [ ! -f "$I/lib/libfdk-aac.a" ]; then
    rm -rf "$W/fdk-aac-2.0.3"
    tar -C "$W" -xzf "$B/dl/fdk-aac/fdk-aac-2.0.3.tar.gz"
    cd "$W/fdk-aac-2.0.3"
    ./configure --host=arm-buildroot-linux-musleabi CC="${X}gcc" CXX="${X}g++" AR="${X}ar" RANLIB="${X}ranlib" \
        --prefix="$I" --enable-static --disable-shared CFLAGS=-O2 CXXFLAGS="-O2 -fno-exceptions -fno-rtti" \
        > "$W/fdk-configure.log"
    make -j"$J" > "$W/fdk-build.log"
    make install > "$W/fdk-install.log"
fi

if [ ! -f "$I/lib/libavcodec.a" ]; then
    rm -rf "$W/ffmpeg-6.1.5"
    tar -C "$W" -xJf "$B/dl/ffmpeg/ffmpeg-6.1.5.tar.xz"
    cd "$W/ffmpeg-6.1.5"
    ./configure --prefix="$I" --enable-cross-compile --cross-prefix="$C" --arch=arm --cpu=arm1176jzf-s \
        --target-os=linux --disable-vfp --disable-neon --disable-armv6t2 --enable-static --disable-shared \
        --disable-programs --disable-doc --disable-everything --disable-network --disable-autodetect \
        --disable-debug --enable-small --enable-decoder=h264,aac,aac_latm --enable-parser=h264,aac,aac_latm \
        --enable-demuxer=mpegts,h264,loas,aac --enable-protocol=file,pipe --enable-swscale --enable-swresample \
        --disable-avdevice --disable-avfilter --disable-postproc --extra-cflags=-O2 > "$W/configure.log"
    make -j"$J" > "$W/build.log"
    make install > "$W/install.log"
fi

cd "$R/src/tv"
for f in bftvd siano bftv; do
    "${C}gcc" -O2 -Wall -Wextra -Wno-unused-function -I"$I/include" -c $f.c -o "$W/$f.o"
done
"${X}g++" -o "$W/bftvd" "$W/bftvd.o" "$W/siano.o" -L"$I/lib" -lavformat -lavcodec -lavutil -lfdk-aac -lm -lpthread -static
"${C}gcc" -o "$W/bftv" "$W/bftv.o" "$W/siano.o" -static
"${C}strip" "$W/bftvd" "$W/bftv"
ls -la "$W/bftvd" "$W/bftv"
echo "copie para o cartão: BigFoston/tv/bftvd (e o firmware: tools/bigfoston/voice-extract.py --tvfw)"
