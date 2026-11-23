#!/bin/sh
# build-web.sh — compila o motor do app Internet (bfweb: NetSurf 3.11 com o frontend
# src/web/frontend) e monta a pasta que vai para o cartão: BigFoston/web/
#
#   tools/bigfoston/build-web.sh host   -> firmware/build/web/host/dist (PC, para o simulador)
#   tools/bigfoston/build-web.sh arm    -> firmware/build/web/arm/dist  (FOSTON)
#
# arm precisa da staging do Buildroot separada do firmware (firmware/build/out-web,
# configs/bigfoston_web_defconfig): libcurl+OpenSSL, libpng, libjpeg-turbo, webp,
# freetype, DejaVu e certificados. Nada disso entra no initrd.
# O tarball (firmware/build/dl/netsurf/netsurf-all-3.11.tar.gz) é conferido pelo SHA-512
# (o mesmo do Alpine Linux; o site do NetSurf não publica soma).
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
MODE=${1:-host}
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$R/firmware/build
W=$B/web/$MODE
TGZ=$B/dl/netsurf/netsurf-all-3.11.tar.gz
SHA512=f931687e08976ca336762587cc1ac75e45518be62b19d1d31aac3063b92b9666f8a022ff3a183e80c89404ba8ff33458b26521130e96943043085a42291ee283
J=$(nproc 2>/dev/null || echo 4)
NS=$W/netsurf-all-3.11
PFX=$W/inst

case $MODE in
host)
    HOSTT=$(cc -dumpmachine)
    PKGC=pkg-config
    STRIP=strip
    EXTRA=""
    ;;
arm)
    OW=$B/out-web
    [ -x "$OW/host/bin/arm-buildroot-linux-musleabi-gcc" ] || { echo "falta $OW (Buildroot bigfoston_web_defconfig)"; exit 1; }
    HOSTT=arm-buildroot-linux-musleabi
    # o buildsystem do NetSurf resolve o compilador com realpath; o do Buildroot é um
    # link para o toolchain-wrapper, que precisa ser chamado pelo próprio nome
    X=$W/cross/bin
    mkdir -p "$X"
    for t in gcc cpp; do
        printf '#!/bin/sh\nexec "%s" "$@"\n' "$OW/host/bin/$HOSTT-$t" > "$X/$HOSTT-$t"
        chmod +x "$X/$HOSTT-$t"
    done
    for t in ar ranlib strip nm objcopy ld readelf; do ln -sf "$OW/host/bin/$HOSTT-$t" "$X/$HOSTT-$t"; done
    export PATH="$X:$OW/host/bin:$PATH"
    PKGC=$OW/host/bin/pkg-config
    STRIP=$OW/host/bin/arm-buildroot-linux-musleabi-strip
    # $ORIGIN/lib: as bibliotecas ficam ao lado do binário, no cartão
    EXTRA='-Wl,-rpath,\$$ORIGIN/lib'
    ;;
*) echo "uso: $0 host|arm"; exit 2 ;;
esac

[ -f "$TGZ" ] || { echo "falta $TGZ (download.netsurf-browser.org/netsurf/releases/source-full/)"; exit 1; }
echo "$SHA512  $TGZ" | sha512sum -c --quiet - || { echo "SHA-512 do NetSurf não confere"; exit 1; }

mkdir -p "$W"
if [ ! -d "$NS" ]; then
    tar -C "$W" -xzf "$TGZ"
fi
# o NetSurf aceita só os frontends da lista: inclui o nosso
sed -i 's/^VLDTARGET := amiga atari beos framebuffer gtk monkey riscos windows$/VLDTARGET := amiga atari beos bigfoston framebuffer gtk monkey riscos windows/' \
    "$NS/netsurf/frontends/Makefile.hts"
grep -q "bigfoston framebuffer" "$NS/netsurf/frontends/Makefile.hts" || { echo "Makefile.hts mudou"; exit 1; }
# o frontend vem do repositório (sempre a versão atual)
rm -rf "$NS/netsurf/frontends/bigfoston"
mkdir -p "$NS/netsurf/frontends/bigfoston"
cp "$R"/src/web/frontend/*.c "$R"/src/web/frontend/*.h "$R"/src/web/frontend/Makefile* "$NS/netsurf/frontends/bigfoston/"
mkdir -p "$NS/netsurf/frontends/bigfoston/res"
for f in adblock.css default.css internal.css quirks.css credits.html licence.html welcome.html netsurf.png favicon.png; do
    cp "$NS/netsurf/frontends/framebuffer/res/$f" "$NS/netsurf/frontends/bigfoston/res/"
done

# host: bibliotecas do NetSurf em $PFX; arm: dentro da staging do out-web
# (PREFIX=/usr DESTDIR=sysroot), porque o pkg-config do Buildroot prefixa o sysroot
if [ "$MODE" = arm ]; then
    SYSROOT=$OW/host/arm-buildroot-linux-musleabi/sysroot
    LPFX=/usr
    LDEST=$SYSROOT
    STAMPS=$PFX
else
    LPFX=$PFX
    LDEST=
    STAMPS=$PFX
    export PKG_CONFIG_PATH="$PFX/lib/pkgconfig"
fi
export PKG_CONFIG="$PKGC"
NOSURF="NSFB_VNC_AVAILABLE=no NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no NSFB_WLD_AVAILABLE=no"
LOG=$W/build.log
: > "$LOG"
mkdir -p "$PFX/include" "$PFX/lib" "$PFX/bin"
cd "$NS"
for L in buildsystem libnslog libwapcaplet libparserutils libcss libhubbub libdom libnsbmp libnsgif \
         libnsutils libutf8proc libnspsl libsvgtiny libnsfb; do
    if [ ! -f "$STAMPS/.done-$L" ]; then
        echo "  LIB $L"
        # shellcheck disable=SC2086
        make -j"$J" install --directory="$L" HOST="$HOSTT" PREFIX="$LPFX" DESTDIR="$LDEST" NSSHARED="$NS/buildsystem" Q=@ \
            WARNFLAGS='-Wall -W -Wno-error' $NOSURF >> "$LOG" 2>&1 || { tail -30 "$LOG"; exit 1; }
        touch "$STAMPS/.done-$L"
    fi
done
if [ ! -f "$PFX/.done-nsgenbind" ]; then
    echo "  TOOL nsgenbind"
    make -j"$J" install --directory=nsgenbind HOST="$(cc -dumpmachine)" PREFIX="$PFX" DESTDIR= NSSHARED="$NS/buildsystem" Q=@ >> "$LOG" 2>&1 \
        || { tail -30 "$LOG"; exit 1; }
    touch "$PFX/.done-nsgenbind"
fi
export PATH="$PATH:$PFX/bin"
echo "  NETSURF bfweb ($MODE)"
NSCC=cc
[ "$MODE" = arm ] && NSCC=$HOSTT-gcc
make -j"$J" --directory=netsurf TARGET=bigfoston HOST="$HOSTT" CC="$NSCC" BUILD_CC=cc Q=@ \
    WARNFLAGS='-W -Wall -Wundef -Wpointer-arith -Wcast-align -Wwrite-strings -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wnested-externs -Wno-error' \
    BFWEB_LDFLAGS="$EXTRA" > "$W/netsurf.log" 2>&1 || { grep -E "error|Error|\*\*\*" "$W/netsurf.log" | tail -30; exit 1; }

# ---- bfdl: o baixador (só libcurl)
echo "  BFDL"
# shellcheck disable=SC2086,SC2046
$NSCC -std=gnu11 -O2 -Wall -Wextra -o "$W/bfdl" "$R/src/web/bfdl.c" $($PKGC --cflags --libs libcurl) \
    $([ "$MODE" = arm ] && echo '-Wl,-rpath,$ORIGIN/lib') || exit 1

# ---- pasta do cartão: BigFoston/web/
D=$W/dist
rm -rf "$D"
mkdir -p "$D/res/fonts" "$D/lib"
cp netsurf/bfweb "$D/bfweb"
cp "$W/bfdl" "$D/bfdl"
"$STRIP" "$D/bfweb" "$D/bfdl"
for f in netsurf/frontends/bigfoston/res/*.css netsurf/frontends/bigfoston/res/*.html netsurf/frontends/bigfoston/res/*.png; do
    cp "$f" "$D/res/"
done
cp netsurf/frontends/bigfoston/res/en/Messages "$D/res/Messages"
cp "$R/src/web/frontend/res/Choices" "$R/src/web/frontend/res/Messages.pt" "$D/res/"
if [ "$MODE" = arm ]; then
    T=$OW/target
    for f in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSans-Oblique.ttf DejaVuSans-BoldOblique.ttf \
             DejaVuSerif.ttf DejaVuSerif-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
        cp "$T/usr/share/fonts/dejavu/$f" "$D/res/fonts/"
    done
    cp "$T/etc/ssl/certs/ca-certificates.crt" "$D/res/ca-bundle.crt"
    # bibliotecas que o rootfs do firmware não tem (libz e libexpat já estão lá)
    for n in $(readelf -d "$D/bfweb" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        case $n in libc.so|libm.so|libz.so.1|libexpat.so.1) continue ;; esac
        cp -L "$T/usr/lib/$n" "$D/lib/" 2>/dev/null || cp -L "$T/lib/$n" "$D/lib/"
    done
    # dependências das dependências (libssl -> libcrypto, freetype -> png...)
    for pass in 1 2 3; do
        for so in "$D"/lib/*.so*; do
            for n in $(readelf -d "$so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
                case $n in libc.so|libm.so|libz.so.1|libexpat.so.1) continue ;; esac
                [ -f "$D/lib/$n" ] || cp -L "$T/usr/lib/$n" "$D/lib/" 2>/dev/null || cp -L "$T/lib/$n" "$D/lib/"
            done
        done
    done
    "$STRIP" "$D"/lib/*.so*
else
    for f in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSans-Oblique.ttf DejaVuSans-BoldOblique.ttf \
             DejaVuSerif.ttf DejaVuSerif-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
        p=$(fc-match -f '%{file}' "$(basename "$f" .ttf | sed 's/-/:style=/')" 2>/dev/null || true)
        [ -f "/usr/share/fonts/TTF/$f" ] && p=/usr/share/fonts/TTF/$f
        [ -n "$p" ] && cp "$p" "$D/res/fonts/$f"
    done
    for c in /etc/ssl/certs/ca-certificates.crt /etc/ca-certificates/extracted/tls-ca-bundle.pem; do
        [ -f "$c" ] && { cp "$c" "$D/res/ca-bundle.crt"; break; }
    done
fi
du -sh "$D" "$D/bfweb" "$D/lib" 2>/dev/null
echo "pronto: $D"
