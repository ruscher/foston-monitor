# Auditoria dos projetos (24/11/2026)

## Big Remote Play (`../big-remote-play`, `main` em `dbaed0f`)

- **O que é:** Python 3 com GTK4/libadwaita. Não captura nem codifica vídeo: configura e sobe o
  Sunshine e o Moonlight.
- **Protocolo:** nada próprio. Tudo é Sunshine/GameStream padrão: pareamento, lista de apps,
  entradas por monitor, controle, teclado, mouse e toque.
  - O único protocolo do Big Remote Play é uma descoberta por UDP na porta 48011, sem
    autenticação.
- **Portas:**
  - 47989 HTTP e 47984 HTTPS (pareamento);
  - 48010 RTSP;
  - UDP 47998/47999/48000 (vídeo, controle e áudio);
  - 47990: painel web, só em 127.0.0.1.
- **Pareamento:** o PIN padrão do Moonlight. O cliente inventa os 4 dígitos e o usuário aprova no
  computador.
  - O Big Remote Play consulta `GET /api/pin` e aprova com `POST /api/pin`.
  - Nomes genéricos (`roth`, `moonlight`) não aparecem na aprovação. Por isso o `bfrp` manda
    `devicename=FOSTON`.
- **Monitores:** uma entrada por tela no `apps.json` (`NN - Monitor` com `output-name`), lidas
  pelo Sunshine com o patch do Big Remote Play (o `/usr/bin/sunshine` deste PC tem o patch).
  Trocar de tela é sair e abrir outra entrada: o Sunshine não troca a tela no meio da sessão.
- **Entrada fraca:** a entrada `Desktop Low` tem `max-bitrate` 3000. Não existe perfil 800×480; o
  Sunshine escala para o que o cliente pede.
- **Modo só controle e agente de entrada no PC:** não existem.
  - A opção de um canal separado celular→PC foi rejeitada pelo projeto
    (`docs/touch/03-arquitetura.md`).
  - O caminho é uma sessão normal do Sunshine com o vídeo ignorado. O `bfrp` faz isso
    ([22](22-controller-only-mode.md)).
- **Controles do celular (fork `moonlight-android-touch`, `brp/native-touch`):**
  - Ctrl/Alt/Shift/Win com "tocar para travar" (`ModifierKeys.java`), soltos ao fechar a barra,
    abrir o menu ou terminar a sessão;
  - layouts Pad, Shooter, Racing, Fighting e Classic;
  - presets de qualidade (`QualityPresets.java`).
- **Recomendação do projeto:** soltar tudo no início, no fim e na troca, porque o Sunshine
  2026.914 guarda botões presos depois de uma queda.
- **Licenças:** GPL-3.0-or-later (Big Remote Play) e GPL-3.0 (Moonlight e fork). O `bfrp` também
  é GPLv3.
- **Ícone:** `usr/share/icons/hicolor/scalable/apps/br.com.biglinux.remoteplay.svg`.

## Moonlight Embedded (`firmware/build/dl/moonlight/moonlight-embedded`, `33142e5`)

| Parte | Dependências | Uso no FOSTON |
|---|---|---|
| `libgamestream` (`client.c`, `http.c`, `mkcert.c`, `xml.c`) | curl, OpenSSL, expat, libuuid | **usada** |
| `libgamestream/discover.c` | avahi | fora: pelo USB o PC é 192.168.7.1 |
| `third_party/moonlight-common-c` (protocolo, enet, nanors) | OpenSSL | **usada**, sem alteração |
| programa `moonlight` | libevdev, udev, SDL/FFmpeg/X11/Pulse/CEC, Opus | não usado: peso e backends de outros SoCs |

- **curl, OpenSSL e expat:** já existiam para ARM (as do navegador, em `firmware/build/out-web`).
- **libuuid:** trocada por um cabeçalho de 30 linhas (`src/remoteplay/uuid/uuid.h`).
- **Ajustes na cópia da libgamestream** (`build-remoteplay.sh`):
  - `encrypt`/`decrypt` → `gs_encrypt`/`gs_decrypt` (a musl declara `encrypt()`);
  - `devicename=roth` → `FOSTON`.

## BigFoston: o que é reaproveitado

| Componente | Uso |
|---|---|
| `core/gamepad.c` + `padmap.c` (SDL GameControllerDB) | o controle Bluetooth já mapeado, com `gamepad_capture` |
| pacotes `.bfapp` e grupo Jogos | o app e o motor num pacote só |
| bibliotecas do navegador no cartão (`BigFoston/web/lib`) | curl, OpenSSL e expat do motor (rpath) |
| `src/tv/bftvd.c` (H.264 + RGB565 no framebuffer) | base para o vídeo (fase 6) |
| rede USB CDC-ECM | o caminho principal até o PC |
