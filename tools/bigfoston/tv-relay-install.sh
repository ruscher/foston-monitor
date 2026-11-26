#!/bin/sh
# tv-relay-install.sh — instala no PC o relay da TV do FOSTON (docs/tv/08-relay-pc.md).
# Com o FOSTON na rede USB, o PC decodifica a TV: o bftvd manda o TS da antena, o relay
# decodifica o H.264 inteiro (30 qps, com o filtro de desblocagem) e o HE-AAC, e devolve quadros
# prontos e som; o FOSTON só desenha e toca. Sem o relay (ou sem o PC), o FOSTON decodifica
# sozinho, como antes.
#
#   tools/bigfoston/tv-relay-install.sh          compila, instala e liga (serviço do usuário, systemd)
#   tools/bigfoston/tv-relay-install.sh remove   desliga e remove
#
# Precisa de ffmpeg (libavformat, libavcodec, libswscale, libswresample) e lz4 com os cabeçalhos.
# O relay conecta no FOSTON (192.168.7.2:48122) quando a TV está tocando: nenhuma porta nova é
# aberta no PC.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$R/firmware/build/tv-relay
BIN=$HOME/.local/bin/bigfoston-tv-relay
UNIT=$HOME/.config/systemd/user/bigfoston-tv-relay.service
if [ "${1:-}" = remove ]; then
    systemctl --user disable --now bigfoston-tv-relay.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN"
    systemctl --user daemon-reload
    echo "relay da TV removido"
    exit 0
fi
mkdir -p "$OUT" "$HOME/.local/bin" "$(dirname "$UNIT")"
cd "$R/src/tv"
# shellcheck disable=SC2046
cc -O2 -Wall -Wextra -DYUVZ_ENCODER bftv-relay.c yuvz.c ../ui/core/lz4dec.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample liblz4) -lpthread \
    -o "$OUT/bftv-relay"
install -m 755 "$OUT/bftv-relay" "$BIN"
cat > "$UNIT" <<UNIT
[Unit]
Description=TV do FOSTON decodificada no PC (relay de vídeo e som pela rede USB)
After=graphical-session.target

[Service]
ExecStart=$BIN -v
Restart=always
RestartSec=2
Nice=-2

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now bigfoston-tv-relay.service
systemctl --user restart bigfoston-tv-relay.service
systemctl --user --no-pager status bigfoston-tv-relay.service | head -5
