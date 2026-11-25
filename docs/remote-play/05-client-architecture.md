# Arquitetura do cliente

```
 shell (LVGL, scr_brp.c)  ── stdin/stdout, linhas ──>  bfrp (pacote do app)  ── rede USB ──>  Sunshine
   telas, controle virtual,                               libgamestream (HTTP/HTTPS,
   controle Bluetooth                                     pareamento, apps)
   (gamepad_capture)                                      moonlight-common-c (RTSP, ENet
                                                          de controle cifrado, vídeo e áudio)
```

O motor é um processo separado, como o `bfweb` e o `bftvd`:
- uma falha no protocolo não derruba a interface;
- a interface não carrega OpenSSL e curl.

O shell inicia `<dados>/apps/big-remote-play/bfrp --keys <dados>/remoteplay` ao abrir a tela e o
encerra ao sair.

## Protocolo shell ↔ bfrp

| Shell → bfrp | bfrp → shell |
|---|---|
| — | `READY` |
| `HOST endereço` | `HOST endereço\|pareado\|ocupado\|app_atual\|versão` ou `ERR host …` |
| `PAIR` | `PIN nnnn`, depois `PAIR ok` ou `PAIR erro …` |
| `UNPAIR` | `UNPAIR ok` |
| `APPS` | `APP id\|nome` … `APPS fim` |
| `START app L A qps kbps vídeo` | `STAGE …`, `VIDEO LxA qps`, `STARTED`, `STATUS boa\|ruim`, `TERMINATED n` |
| `STOP` / `QUITAPP` | `STOPPED` / `QUITAPP ok` |
| `PAD botões lt rt lx ly rx ry` | — (estado inteiro do controle 1) |
| `KEY vk 1\|0 mods`, `MREL dx dy`, `MBTN b 1\|0`, `SCROLL n` | — |
| `RELEASE` | — (solta botões, eixos, teclas e mouse) |

- Nomes vindos do host perdem quebras de linha e o `|`.
- Os valores numéricos são limitados (eixos, gatilhos, resolução 320–1920, 10–60 qps,
  300–20000 kbps).

## Tempo e memória medidos

| Medida | Valor |
|---|---|
| Primeira execução: gerar o certificado do cliente | alguns segundos (uma vez) |
| `HOST` pelo USB até a resposta | < 1 s |
| `bfrp` | 194 KB (pacote: 103 KB) |
