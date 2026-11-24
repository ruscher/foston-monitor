#!/bin/sh
# build-remoteplay.sh — compila o motor do Big Remote Play (bfrp: src/remoteplay/bfrp.c) para o
# FOSTON, com a libgamestream e a moonlight-common-c do Moonlight Embedded (GPLv3).
#
#   tools/bigfoston/build-remoteplay.sh        -> firmware/build/remoteplay/bfrp e o pacote
#                                                 pkg/apps/big-remote-play.bfapp (com o motor)
#
# Usa o toolchain e a staging do navegador (firmware/build/out-web: libcurl, OpenSSL, expat):
# as mesmas bibliotecas que já vão no cartão em BigFoston/web/lib. O bfrp vai para o cartão em
# BigFoston/remoteplay/bfrp (docs/remote-play).
# Fontes: firmware/build/dl/moonlight/moonlight-embedded (git clone --recursive de
# github.com/moonlight-stream/moonlight-embedded). Fica de fora a descoberta por avahi
# (discover.c): pela rede USB o PC é 192.168.7.1, e o endereço pode ser digitado.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$R/firmware/build
M=$B/dl/moonlight/moonlight-embedded
C=$M/third_party/moonlight-common-c
OW=$B/out-web
CC=$OW/host/bin/arm-buildroot-linux-musleabi-gcc
SR=$OW/host/arm-buildroot-linux-musleabi/sysroot
W=$B/remoteplay
[ -x "$CC" ] || { echo "falta o toolchain $OW (Buildroot bigfoston_web_defconfig)"; exit 1; }
[ -f "$M/libgamestream/client.c" ] || { echo "falta $M (git clone --recursive moonlight-embedded)"; exit 1; }
mkdir -p "$W/gs"

# libgamestream: o nome do aparelho no pedido de pareamento ("roth" é ignorado pelo Big Remote
# Play, que mostra o nome ao aprovar o PIN)
for f in client.c http.c mkcert.c xml.c; do
    # encrypt/decrypt: a musl declara encrypt() no unistd.h (conflito com _GNU_SOURCE)
    sed -e 's/devicename=roth/devicename=FOSTON/g' -e 's/\bencrypt(/gs_encrypt(/g' -e 's/\bdecrypt(/gs_decrypt(/g' \
        "$M/libgamestream/$f" > "$W/gs/$f"
done
# o log de depuração (BFRP_DEBUG) vai para o stderr: o stdout é o protocolo com o shell
sed -i 's/printf("/fprintf(stderr, "/g' "$W/gs/http.c"
# uma conexão por pedido (como a libgamestream faz no FreeBSD): o relay fica aberto o tempo todo e
# uma conexão HTTPS parada travava o servidor do Sunshine para todos os outros pedidos
sed -i -e '/^#ifdef __FreeBSD__$/{N;N;s/#ifdef __FreeBSD__\n\(.*FORBID_REUSE.*\)\n#endif/\1/}' "$W/gs/http.c"
cp "$M"/libgamestream/*.h "$W/gs/"

CFLAGS="-O2 -march=armv6 -mfloat-abi=soft -pipe -D_GNU_SOURCE -DHAS_SOCKLEN_T -DHAS_POLL=1 -DHAS_FCNTL=1 \
 -DHAS_IOCTL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 \
 -DHAS_MSGHDR_FLAGS=1 -DNDEBUG -Wno-deprecated-declarations"
FF=$B/ffmpeg-tv/inst                      # FFmpeg mínimo da TV (build-tv.sh): o mesmo H.264
[ -f "$FF/lib/libavcodec.a" ] || { echo "falta $FF (tools/bigfoston/build-tv.sh)"; exit 1; }
OP=$B/opus-arm/inst                       # libopus 1.6.1 em ponto fixo (som do Sunshine), ver docs/remote-play/10
[ -f "$OP/lib/libopus.a" ] || { echo "falta $OP: configure da opus-1.6.1 com --enable-fixed-point --disable-intrinsics"; exit 1; }
LZ=$B/dl/lz4/lz4-1.10.0/lib                # o descompressor oficial do LZ4 (BSD-2): rápido no ARM11
[ -f "$LZ/lz4.c" ] || { echo "falta $LZ (lz4-1.10.0.tar.gz conferido pelo hash do pacote lz4 do Buildroot)"; exit 1; }
INC="-I$LZ -I$OP/include -I$FF/include -I$R/src/remoteplay -I$W/gs -I$C/src -I$C/enet/include -I$C/nanors -I$C/nanors/deps -I$C/nanors/deps/obl \
 -I$SR/usr/include"
SRC="$R/src/remoteplay/bfrp.c $R/src/remoteplay/video.c $R/src/remoteplay/audio.c $R/src/remoteplay/client.c $B/dl/lz4/lz4-1.10.0/lib/lz4.c $W/gs/client.c $W/gs/http.c $W/gs/mkcert.c $W/gs/xml.c \
 $(ls $C/src/*.c) $C/enet/callbacks.c $C/enet/compress.c $C/enet/host.c $C/enet/list.c $C/enet/packet.c \
 $C/enet/peer.c $C/enet/protocol.c $C/enet/unix.c $C/nanors/rs.c $C/nanors/deps/obl/oblas_common.c \
 $C/nanors/deps/obl/oblas_lite.c"
# shellcheck disable=SC2086
"$CC" $CFLAGS $INC $SRC -o "$W/bfrp" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" "$OP/lib/libopus.a" -L"$SR/usr/lib" -lcurl -lssl -lcrypto -lexpat -latomic -lpthread -lm \
    -Wl,-rpath,/mnt/sd/BigFoston/web/lib 2> "$W/build.log" || { grep -E "error" "$W/build.log" | head -30; exit 1; }
"$OW/host/bin/arm-buildroot-linux-musleabi-strip" "$W/bfrp"
ls -la "$W/bfrp"

# o relay do PC (o mesmo bfrp, decodificando e reduzindo lá): firmware/build/remoteplay/bfrp-relay
# com as bibliotecas do sistema (FFmpeg, libopus, liblz4, curl, OpenSSL, expat); instalar com
# tools/bigfoston/brp-relay-install.sh
HSRC="$R/src/remoteplay/bfrp.c $R/src/remoteplay/relay.c $W/gs/client.c $W/gs/http.c $W/gs/mkcert.c $W/gs/xml.c \
 $(ls $C/src/*.c) $C/enet/callbacks.c $C/enet/compress.c $C/enet/host.c $C/enet/list.c $C/enet/packet.c \
 $C/enet/peer.c $C/enet/protocol.c $C/enet/unix.c $C/nanors/rs.c $C/nanors/deps/obl/oblas_common.c \
 $C/nanors/deps/obl/oblas_lite.c"
# shellcheck disable=SC2086
gcc -O2 -pipe -D_GNU_SOURCE -DBFRP_RELAY -DHAS_SOCKLEN_T -DHAS_POLL=1 -DHAS_FCNTL=1 -DHAS_IOCTL=1 -DHAS_GETADDRINFO=1 \
    -DHAS_GETNAMEINFO=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DNDEBUG -Wno-deprecated-declarations \
    -I"$R/src/remoteplay" -I"$W/gs" -I"$C/src" -I"$C/enet/include" -I"$C/nanors" -I"$C/nanors/deps" -I"$C/nanors/deps/obl" \
    $(pkg-config --cflags libavcodec libswscale opus liblz4) $HSRC -o "$W/bfrp-relay" \
    $(pkg-config --libs libavcodec libavutil libswscale opus liblz4 libcurl openssl expat) -lpthread -lm 2> "$W/build-relay.log" \
    || { grep -E "error" "$W/build-relay.log" | head -30; exit 1; }
ls -la "$W/bfrp-relay"

# o pacote do app: apps/big-remote-play com o motor dentro -> pkg/apps/big-remote-play.bfapp
P=$W/pkg/big-remote-play
rm -rf "$W/pkg"
mkdir -p "$P"
cp "$R"/apps/big-remote-play/* "$P/"
cp "$W/bfrp" "$P/bfrp"
mkdir -p "$R/pkg/apps"
python3 "$R/tools/bigfoston/bfapp.py" build "$P" -o "$R/pkg/apps"
ls -la "$R/pkg/apps/big-remote-play.bfapp"
