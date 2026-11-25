#!/bin/sh
# brp-relay-install.sh — instala no PC o relay do Big Remote Play para o FOSTON (docs/remote-play/34).
# O relay é o cliente do Sunshine no próprio PC: decodifica o vídeo (60 qps), reduz ao tamanho da tela
# do FOSTON e manda pela rede USB; o FOSTON só desenha. Sem ele, o FOSTON decodifica sozinho (lento).
#
#   tools/bigfoston/brp-relay-install.sh          instala e liga (serviço do usuário, systemd)
#   tools/bigfoston/brp-relay-install.sh remove   desliga e remove
#
# As chaves de pareamento são as do FOSTON (o Sunshine reconhece o cliente pelo certificado):
# copiadas uma vez de /data/bigfoston/remoteplay pela rede USB (root@192.168.7.2) para
# ~/.config/bigfoston-brp-relay (0700). O relay conecta no FOSTON (192.168.7.2:48120) quando o app
# está aberto: nenhuma porta nova é aberta no PC.
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
R=$(cd "$(dirname "$0")/../.." && pwd)
BIN=$HOME/.local/bin/bigfoston-brp-relay
KEYS=$HOME/.config/bigfoston-brp-relay
UNIT=$HOME/.config/systemd/user/bigfoston-brp-relay.service
if [ "${1:-}" = remove ]; then
    systemctl --user disable --now bigfoston-brp-relay.service 2>/dev/null || true
    rm -f "$UNIT" "$BIN"
    systemctl --user daemon-reload
    echo "relay removido (as chaves ficam em $KEYS)"
    exit 0
fi
[ -x "$R/firmware/build/remoteplay/bfrp-relay" ] || { echo "falta o relay: tools/bigfoston/build-remoteplay.sh"; exit 1; }
mkdir -p "$HOME/.local/bin" "$(dirname "$UNIT")" "$KEYS"
chmod 700 "$KEYS"
if [ ! -s "$KEYS/key.pem" ]; then
    for f in client.pem key.pem client.p12 uniqueid.dat; do
        ssh -o BatchMode=yes root@192.168.7.2 "cat /data/bigfoston/remoteplay/$f" > "$KEYS/$f"
    done
    chmod 600 "$KEYS"/*
fi
install -m 755 "$R/firmware/build/remoteplay/bfrp-relay" "$BIN"
cat > "$UNIT" <<UNIT
[Unit]
Description=Big Remote Play para o FOSTON (relay de vídeo e som pela rede USB)
After=graphical-session.target

[Service]
ExecStart=$BIN --keys $KEYS
Restart=always
RestartSec=2
Nice=-2

[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now bigfoston-brp-relay.service
systemctl --user --no-pager status bigfoston-brp-relay.service | head -5
