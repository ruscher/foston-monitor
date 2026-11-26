# 01 — Auditoria do repositório para o BigFoston OS Installer

Data: 25/11/2026. Base: `main` em `bf0f68f` (BigFoston OS 0.5.1).

## Branches

| Branch | Situação | Conclusão |
|---|---|---|
| `main` | 0.5.1: grupos de apps, Dual Boot (bfagent 0.7.0), Big Remote Play, Jogos/.bfapp, TV, energia | **referência** |
| `bigfoston-linux` | 0 commits fora do `main` | já contida no `main` |
| `big-foston-os` | 1 commit fora do `main`: o merge do PR #1 (conteúdo já no `main`) | já contida no `main` |

Nada a recuperar das outras branches: o instalador parte do `main`.

## Como o BigFoston OS chega ao aparelho hoje

1. **Primeira vez (pelo cartão)**: no CE, *Ajustes → Caminho da navegação →
   `BigFoston\bfagent.exe`* no cartão; o botão **Navegação** abre o agente.
2. **Com o agente aberto e o cabo USB no PC** (`tools/bigfoston/bfagent.py`):
   - `install-agent`: copia o agente para `\ResidentFlash\BigFoston\bfagent.exe` (SHA-256 conferido);
   - `autostart on`: grava `HKLM\init\Launch90` = caminho do agente e `Depend90` = `14 00 32 00`;
   - `install-linux`: copia `zImage`, `initrd.img`, `fs707dc.dtb` e `SHA256SUMS` para
     `\ResidentFlash\BigFoston\linux` (cada arquivo relido e conferido);
   - `autoboot on --delay N`: grava `\ResidentFlash\BigFoston\autoboot.conf` (`delay=N`).
3. **Ao ligar**: o CE abre o agente (Launch90) → `apply_sd_update()` aplica
   `\SDMMC\BigFoston\update` (se houver, conferindo `SHA256SUMS`, mantendo `*.old`, registrando
   `update.log`, renomeando para `update.ok`/`update.bad`) → tela Dual Boot com contagem →
   `bf_boot()` carrega o Linux de `\ResidentFlash\BigFoston\linux` na RAM. O reset devolve o CE.
4. **Atualizações**: `tools/bigfoston/linux-update.py stage` (do Linux rodando, por SSH) grava
   `BigFoston/update.tmp` no cartão, confere e renomeia para `BigFoston/update`; ao ligar, o
   agente aplica. Validado várias vezes (0.5.0 e 0.5.1 em 25/11/2026).

O Linux nunca grava a NAND; só o agente, pelo sistema de arquivos do próprio CE, e só na
partição de dados (`\ResidentFlash`). EBoot, imagem do CE e partições do sistema intocados.

## Peças reaproveitáveis

| Peça | Onde | Estado |
|---|---|---|
| Protocolo BFP + cliente Python (info, ls, get, put, reg, autostart, usb-default, install-agent, install-linux, autoboot, shot, quit) | `tools/bigfoston/bfagent.py`, `bfp.py` | validado no aparelho |
| Aplicar atualização do cartão (conferência, `.old`, log) | `src/wince/agent/bfagent.c` `apply_sd_update()` | validado |
| Tela Dual Boot (cartões, contagem com barra, registro) | `src/wince/agent/ui.c` | validado (0.7.0) |
| Verificação de importações do coredll | `src/wince/agent/tools/check_imports.py` | novo, validado |
| Atualização pelo Linux | `tools/bigfoston/linux-update.py` | validado |
| Gravar o cartão num leitor e conferir por SHA-256 | `tools/bigfoston/sd-install.sh` | **quebrado** (ver abaixo) |
| Formatação e teste do cartão | `format-sd.sh`, `sd-write-test.sh`, `fix-fake-sd.sh` | scripts de bancada |
| Documentação de recuperação | `docs/17-recuperacao.md`, `docs/dual-boot.md` | atual |
| Identidade visual | `src/ui/assets/logo/fostonos.svg`, `src/ui/tools/logo_svg.py`, Montserrat, Font Awesome | pronta |
| Fotos reais | `docs/images/foto-*.jpg`, `docs/art/foston-*.png`, `docs/art/passo-*.png` | poucas, sem os menus do CE |

## Problemas encontrados

1. **`sd-install.sh` copia `initrd.gz`**, mas o build só gera `initrd.img` (o
   `post-image.sh` até apaga `initrd.gz`). Resultado: o script falha (ou, com um build antigo,
   deixa um cartão com initrd velho). O nome canônico é **`initrd.img`**, o mesmo do agente,
   do `linux-update.py`, do `boot-linux.sh` e do `SHA256SUMS` do build.
2. **A primeira instalação exige o cabo USB e o terminal** (`bfagent.py`), e o modo USB do
   FOSTON é pouco confiável para arquivos grandes (registrado em `docs/17-recuperacao.md` e na
   memória do projeto: cópia lenta, link que trava após transferências longas, cache do CE).
3. **O agente só aplica `BigFoston\update` quando já está instalado na memória interna**
   (`autoboot_start()` sai se a origem não for `\ResidentFlash\BigFoston`): o mecanismo mais
   confiável que existe (o próprio CE gravando, a partir do cartão) não serve para a primeira
   instalação.
4. **Não há manifesto de versão** dos artefatos (só `SHA256SUMS`): nada impede misturar
   kernel, initrd e DTB de builds diferentes, nem diz para que modelo a imagem é.
5. **Não há fotos dos menus do Windows CE** (Ajustes, USB, Caminho da navegação) para o guia;
   o agente já sabe capturar a tela do CE (`bfagent.py shot --under`), então dá para obtê-las
   do aparelho real em vez de inventar.

## Ambiente de desenvolvimento disponível

- PC: Manjaro/BigLinux com GTK 4.24, Libadwaita 1.10, PyGObject, UDisks2, polkit, `makepkg`.
- virt-manager com duas VMs (`bwm-r64`, `bwm-r65`); **nenhuma VM Windows**. Sem ela, o
  instalador para Windows não pode ser testado nesta máquina.
- Um FOSTON FS-707DC real (o único), com BigFoston OS 0.5.1 e agente 0.7.0 instalados.
