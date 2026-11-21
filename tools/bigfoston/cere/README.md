# cere — análise offline de módulos da ROM do Windows CE (NAND do FOSTON)

Scripts usados nos estudos do USB (`usbfn.dll`) e do GPS (`gps.dll`); só leem o backup
da NAND (`BF_NAND_IMG`, padrão `backup/2026-11-19-nand/DSK9-msflash.img`). A desmontagem
usa Capstone (`pip install capstone` num venv). Nada aqui é código ou dado proprietário.

| Script | Uso |
|---|---|
| `mod.py` | carrega as seções de um módulo das regiões XIP (`module("gps.dll")`) |
| `imps.py`, `impn.py` | imports (por ordinal e com nome) |
| `exps.py`, `allexps.py` | exportações (ex.: conferir se o agente só importa funções que a `coredll` do aparelho tem) |
| `cedis.py`, `ann.py` | desmontagem ARM com os imports anotados |
| `strs.py` | strings ASCII/UTF-16 com endereço |
