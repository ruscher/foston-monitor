# Firmware Windows CE: cópia de referência para engenharia reversa

O firmware original do FOSTON (Windows CE 5.0, drivers da MediaTek, iGO) fica guardado
**dentro do projeto, mas fora do git**, em `backup/`. O repositório é público e esse firmware
tem partes proprietárias: nada dele é publicado. Este documento diz o que existe, como
conferir e como regenerar o índice.

## O que há em `backup/`

| Pasta | Conteúdo | Conferência |
|---|---|---|
| `2026-11-19-nand/` | imagem inteira da NAND interna (DSK9, 132 MB) e as três partições FAT extraídas (`part1..3`) | `DSK9-msflash.img.sha256` |
| `2026-11-19-sd-raw/` | imagem bruta do cartão SD original | `wince-referencia/SHA256SUMS` |
| `2026-11-19-sd-files/`, `2026-11-18-volume/` | arquivos do cartão e do volume USB (`.tar`) | `.sha256` ao lado |
| `2026-11-19-survey/` | levantamento feito no CE (CPU, discos, registro, processos) | — |
| `2026-11-21-linux-instalado/` | o Linux gravado na memória interna em 21/11 | `SHA256SUMS` |
| `wince-referencia/` | índice gerado (abaixo) | — |

As pastas datadas são só leitura. Conferido em 22/11/2026: todas as somas batem.

## Índice (`backup/wince-referencia/`)

Gerado por [tools/bigfoston/wince-ref.py](../tools/bigfoston/wince-ref.py), só lendo o backup:

- `MANIFEST.md`: partições da NAND, regiões XIP da ROM, contagens, o que falta;
- `SHA256SUMS`: todos os arquivos do backup (`cd backup && sha256sum -c wince-referencia/SHA256SUMS`);
- `rom/regiao1-8c003000/` e `rom/regiao2-8c705000/`: `modules.tsv` (nome, tamanho, endereço
  virtual, seções) e `files.tsv` de cada região; seções dos módulos em
  `mod/<módulo>/<rva>.bin` e arquivos da ROM em `files/`;
- `partitions/2..4`: atalhos para as partições FAT extraídas.

| NAND | Conteúdo |
|---|---|
| Partição 1, BINFS 36 MB | ROM XIP do CE: região 1 (43 módulos: `nk.exe`, `coredll.dll`, `filesys.exe`, `gwes.exe`…) e região 2 (142 módulos: drivers e DLLs) |
| Partição 2, FAT16 3 MB | registro |
| Partição 3, FAT32 75 MB | aplicativos (`app`) |
| Partição 4, FAT32 12 MB | `\ResidentFlash`: arquivos do GPS; desde 21/11 também o agente e o Linux instalados |

Módulos de interesse: áudio (`wavedev_drv.dll`, `waveapi.dll`), Bluetooth (`mtk_bthuart.dll`,
`btd.dll`, `bta2dp.dll`, `bthuart.dll`…), TV digital (`SianoSPI.dll`, `SmsGenDrv.dll`), GPS
(`gps.dll`, `gps2.dll`), `uart.dll`, `touch.dll`. Para desmontar uma função pelo texto de
depuração ou pelo endereço: [tools/bigfoston/cedll.py](../tools/bigfoston/cedll.py) (ver
[hardware-enablement/06](hardware-enablement/06-bluetooth-hardware.md)).

## Limites

- Uma seção comprimida (CECompress) em cada região não foi extraída; está listada nos `.tsv`.
- O bootloader EBoot ("JWD B.13D...101025") fica fora da DSK9 e não está na cópia: lê-lo
  exigiria acessar a NAND fora das interfaces do CE.
- A `\ResidentFlash` mudou depois do backup (Linux e agente instalados).
