# 00 — Plano mestre do BigFoston OS Installer

Ver a auditoria em [01-repository-audit.md](01-repository-audit.md).

## A decisão central: o cartão instala, o aparelho grava

O caminho mais seguro que o projeto já validou é **o próprio Windows CE gravar a memória
interna a partir do cartão** (`apply_sd_update()` do agente: confere `SHA256SUMS`, guarda
`.old`, registra `update.log`). Proposta:

1. O instalador do PC só **prepara o cartão** num leitor de cartões (rápido e verificável):
   `BigFoston/bfagent.exe` + `BigFoston/update/{zImage, initrd.img, fs707dc.dtb, SHA256SUMS,
   manifest.json}`, sem apagar nada do usuário e relendo cada arquivo depois de remontar.
2. No FOSTON, uma vez: *Ajustes → Caminho da navegação → BigFoston\bfagent.exe* e tocar em
   **Navegação** (guia com fotos reais do aparelho).
3. O agente (nova versão 0.8.0), aberto pelo cartão, mostra na tela Dual Boot um cartão
   **Instalar o Dual Boot**, com o que vai mudar. Com a confirmação no toque, ele: guarda os
   valores originais (`Launch90`, `Depend90`, caminho da navegação) em
   `BigFoston/recuperacao/original.reg.txt`, copia a si mesmo para `\ResidentFlash\BigFoston`,
   aplica a pasta `update` (o código que já funciona), grava `autoboot.conf` e `Launch90`, e
   confere tudo relendo. **Sem cabo USB, sem terminal.**
4. Desinstalar é o caminho inverso, também no toque: **Restaurar o FOSTON original** apaga
   `Launch90`/`Depend90` (ou devolve os valores guardados), remove só `\ResidentFlash\BigFoston`
   e volta o caminho da navegação; o cartão fica com os arquivos do usuário.

O modo USB (Mass Storage) vira alternativa só para o pequeno (o agente), e o ActiveSync fica
como informação: não há implementação confiável nem testável aqui.

## Fases (cada uma com testes e commit próprio)

| Fase | Entrega | Testável aqui |
|---|---|---|
| 1 | **Núcleo** (`installer/bigfoston_installer/core`, sem GUI): manifesto de versão e de compatibilidade, verificação SHA-256 por streaming, descoberta de cartões pelo UDisks2 com as proteções (nunca o disco do sistema, nunca o FOSTON em modo serial, identidade revalidada antes de gravar), cópia transacional para `update.tmp` → `update` com releitura após remontar, backup com manifesto. Corrige o `sd-install.sh`. | sim: testes unitários + cartões virtuais (loop) |
| 2 | **App Linux** (GTK 4 + Libadwaita): central (Instalar, Atualizar, Reparar, Restaurar, Diagnóstico, Manual), termos com as 4 confirmações, compatibilidade, escolha da versão/arquivo local, seleção assistida do cartão (tirar e pôr), backup, resumo, progresso real, guia do aparelho, conclusão. Temas claro/escuro, gettext (pt-BR, en, es). | sim (sessão gráfica + testes de UI) |
| 3 | **Agente 0.8.0**: "Instalar o Dual Boot" e "Restaurar o FOSTON original" no toque, com os valores originais guardados; opções de inicialização (contagem, padrão). | prévia no PC + aparelho (com sua autorização, porque mexe no registro do único FOSTON) |
| 4 | **Fotos reais dos menus do CE** capturadas pelo agente (`shot --under`) e o guia ilustrado. | aparelho |
| 5 | **Pacotes**: PKGBUILD/`.pkg.tar.zst`, `.desktop`, ícones; pacote offline `BigFoston-OS-FS707DC-<versão>.zip` com manifesto e `SHA256SUMS`; GitHub Actions (testes + pacotes, publicação só manual). | Linux sim; Actions só ao publicar |
| 6 | **Windows** (GTK 4/Libadwaita via MSYS2 UCRT64 + PyInstaller, armazenamento pelo WMI/PowerShell). | **não**: falta uma VM Windows; fica implementado só se houver como testar |

## Regras que valem para todas as fases

- Nunca gravar firmware, EBoot, imagem do CE, partições do sistema nem setores brutos.
- Nunca formatar sem pedido explícito, nunca gravar num disco não identificado, nunca no disco do sistema.
- Nunca dar por instalado o que só foi copiado: o instalador separa "cartão preparado" de
  "instalado e iniciado no aparelho".
- Nada de botão sem função, progresso simulado ou plataforma declarada sem teste.
