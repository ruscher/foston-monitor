# 17 · Recuperação: como voltar ao FOSTON original

Nada do que o BigFoston OS faz hoje grava na NAND interna. O Windows CE, o EBoot e as
partições da flash continuam como saíram de fábrica. Toda a recuperação abaixo é
reversível e não exige abrir o aparelho.

## Situações e o que fazer

| Situação | O que fazer |
|---|---|
| Linux/payload rodando, tela parada ou com faixas | **reset** (furinho com clipe, ou segurar liga/desliga ~10 s): volta ao Windows CE |
| Botão **Navegação** abre o agente em vez do GPS | Ajustes → Caminho da navegação → escolher o navegador original (`7 pole 2012\IGO8.exe` nesta unidade) |
| O PC vê o FOSTON como porta serial (`045e:00ce`) e não como pendrive | no FOSTON: **Ajustes → USB → armazenamento/Mass Storage** |
| Agente não abre / tela branca | reset; conferir `BigFoston\bfagent.exe` no cartão e o hash em `bfagent.exe.sha256` |
| Cartão SD corrompido | o sistema original não depende dele; restaurar os arquivos de `backup/2026-11-18-volume/volume-files.tar` (conferir `files.sha256`) |

## O que está salvo (no PC, fora do git)

| Backup | Conteúdo | Serve para |
|---|---|---|
| `backup/2026-11-18-volume/` | todos os arquivos do cartão SD | restaurar mapas, rotas, contatos BT, iGO |
| `backup/2026-11-19-nand/DSK9-msflash.img` | NAND inteira vista pelo CE: MBR, imagem do Windows CE (BINFS), registro, aplicativos, dados de GPS | referência e, no futuro, regravação |
| `backup/2026-11-19-nand/DSK9-msflash.img.sha256` | hash do dump | verificar integridade |
| `backup/2026-11-19-survey/` | registro completo, mapa de memória, módulos | engenharia reversa |

**Ainda não salvo:** o bootloader (EBoot) e os blocos da NAND antes do MBR, que o
Windows CE não expõe em `DSK9:`. Enquanto isso não existir, **nenhuma escrita na
NAND será feita**.

## Conferir o backup da NAND

```sh
cd backup/2026-11-19-nand
sha256sum -c DSK9-msflash.img.sha256
```

## Regras que o projeto segue

- O agente lê memória física por mapeamento **somente leitura**; não grava registradores
  nem setores brutos.
- O único valor de registro que o agente altera é `DefaultClientDriver`, e apenas para
  **devolver** o valor que o driver USB mudou sozinho.
- Boot do Linux: carregado na RAM a partir do cartão SD; o reset sempre devolve o Windows CE.
