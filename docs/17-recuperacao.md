# 17 · Recuperação: como voltar ao FOSTON original

O Windows CE, o EBoot e as partições do sistema continuam como saíram de fábrica.
Desde 20/11/2026 há **duas gravações pequenas e reversíveis** na memória interna,
autorizadas pelo dono do aparelho:

- o agente em `\ResidentFlash\BigFoston\bfagent.exe` (partição FAT de dados, ~39 KB);
- a entrada `HKLM\init\Launch90` = esse caminho, com `Depend90` = `14 00 32 00`, no
  registro persistente (`system.hv`). Ela faz o agente abrir sozinho no boot; o menu
  original continua funcionando.

Toda a recuperação abaixo é reversível e não exige abrir o aparelho.

## Situações e o que fazer

| Situação | O que fazer |
|---|---|
| Linux/payload rodando, tela parada ou com faixas | **reset** (furinho com clipe, ou segurar liga/desliga ~10 s): volta ao Windows CE |
| Botão **Navegação** abre o agente em vez do GPS | Ajustes → Caminho da navegação → escolher o navegador original (`7 pole 2012\IGO8.exe` nesta unidade) |
| O PC vê o FOSTON como porta serial (`045e:00ce`) e não como pendrive | no FOSTON: **Ajustes → USB → armazenamento/Mass Storage** |
| Desfazer o autostart do agente | com o agente aberto: `bfagent.py autostart off` (apaga `Launch90`/`Depend90`); depois, se quiser, `bfagent.py rm '\ResidentFlash\BigFoston\bfagent.exe'` |
| O PC vê porta serial em vez de pendrive logo no boot | com o agente aberto: `bfagent.py usb-default mass` (o driver USB do CE às vezes grava a troca do agente como padrão de boot) |
| `reboot` (Linux ou `bfagent.py reboot`) **desliga** o aparelho | comportamento do próprio hardware com o watchdog (o CE faz igual): é só ligar no botão; o agente abre sozinho |
| Agente não abre / tela branca | reset; conferir `BigFoston\bfagent.exe` no cartão e o hash em `bfagent.exe.sha256` |
| Cartão SD corrompido | o sistema original não depende dele; restaurar os arquivos de `backup/2026-11-18-volume/volume-files.tar` (conferir `files.sha256`) |

## O que está salvo (no PC, fora do git)

| Backup | Conteúdo | Serve para |
|---|---|---|
| `backup/2026-11-18-volume/` | todos os arquivos do cartão SD (18/11) | restaurar mapas, rotas, contatos BT, iGO |
| `backup/2026-11-19-sd-files/sd-files.tar` | **todos os 2 159 arquivos do cartão antes da formatação** (19/11), com `files.sha256` verificado | restaurar o cartão: montar e `tar -xf` na raiz |
| `backup/2026-11-19-nand/DSK9-msflash.img` | NAND inteira vista pelo CE: MBR, imagem do Windows CE (BINFS), registro, aplicativos, dados de GPS | referência e, no futuro, regravação |
| `backup/2026-11-19-nand/DSK9-msflash.img.sha256` | hash do dump | verificar integridade |
| `backup/2026-11-19-survey/` | registro completo, mapa de memória, módulos | engenharia reversa |

**Ainda não salvo:** o bootloader (EBoot) e os blocos da NAND antes do MBR, que o
Windows CE não expõe em `DSK9:`. Enquanto isso não existir, **nenhuma escrita na
NAND será feita**.

## Restaurar os arquivos originais do cartão SD

O cartão foi reformatado em 19/11/2026 (FAT32, rótulo `BIGFOSTON`). Para voltar os mapas
e dados do iGO, com o FOSTON no menu e o cabo ligado (pendrive):

```sh
M=/run/media/$USER/BIGFOSTON
tar -C "$M" -xf backup/2026-11-19-sd-files/sd-files.tar
(cd "$M" && sha256sum -c --quiet "$OLDPWD/backup/2026-11-19-sd-files/files.sha256")
```

Depois: reset no FOSTON e "Caminho da navegação" de volta para o iGO.

**O cartão original desta unidade está em fim de vida:** lê sem erros, mas as
gravações falham de forma intermitente (pelo FOSTON e também num leitor do PC).
Use um cartão SD novo, de preferência de 2 GB (FAT32). Para preparar um cartão novo:
formate em FAT32 e rode `tools/bigfoston/sd-install.sh` com ele num leitor; para os
mapas do iGO, restaure `backup/2026-11-19-sd-files/sd-files.tar`.

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
