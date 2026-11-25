# Big Remote Play para o FOSTON

Cliente do Sunshine do computador (o mesmo do Big Remote Play do BigLinux). A tela é nativa do
shell (`src/ui/screens/scr_brp.c`); o protocolo fica no motor `bfrp` (`src/remoteplay/bfrp.c`),
montado por `tools/bigfoston/build-remoteplay.sh`, que também gera este pacote com o motor
dentro.

Ícone: o desenho do `br.com.biglinux.remoteplay.svg` do Big Remote Play (GPL-3.0-or-later), sem o
fundo. Motor: libgamestream e moonlight-common-c do Moonlight Embedded (GPL-3.0).

Documentação: `docs/remote-play/`.
