# Etapa 1 — Grupos de apps (categorias)

## O que o usuário vê

Na grade de Apps, depois dos apps do sistema e antes dos WebApps, cada grupo com apps
instalados vira uma **pasta**:
- um quadrado de vidro na cor do grupo, com até 4 miniaturas dos apps, cada uma com o ícone
  ou a inicial na cor do app;
- um selo com o número de apps;
- o nome do grupo embaixo.

Tocar abre a tela do grupo (`SCR_APPGROUP`). Nela, tocar num app abre o app; segurar 2 s
abre o menu (enviar para outro grupo, tirar do grupo, mover, desinstalar...).

**Jogos aparece sempre** na primeira vez, mesmo vazio. Vazio, a tela do grupo explica como
instalar e tem o botão Abrir Arquivos. Os outros grupos de categoria nascem quando o
primeiro app daquela categoria é instalado. O modo quiosque não mostra grupos.

Desde os grupos personalizados, as categorias são grupos comuns com uma `key`: o usuário pode
renomear, recolorir, reordenar ou excluir, e criar os próprios grupos com qualquer app (do
sistema, pacote ou WebApp). A categoria só decide **para onde vai um pacote recém-instalado**.
Ver [launcher-groups-architecture.md](../launcher-groups-architecture.md).

## Categoria → grupo

O grupo sai da primeira categoria principal do freedesktop.org que aparece em
`Categories=`, na ordem em que o app as lista (`Game;Utility;` vai para Jogos):

| `Categories=` | Grupo | Cor | Ícone |
|---|---|---|---|
| Game | **Jogos** | `#7a4cd9` | controle (Font Awesome `gamepad`, novo) |
| AudioVideo, Audio, Video | Multimídia | `#e5534b` | play |
| Network | Internet | `#3584e4` | globo |
| Graphics | Gráficos | `#d6409f` | imagens |
| Office | Escritório | `#c98b2c` | livro |
| Education, Science | Educação | `#2ec27e` | livro |
| Development | Programação | `#4b5363` | terminal |
| Utility | Utilitários | `#1fb5cc` | foguete |
| System, Settings | Sistema | `#6c7383` | engrenagem |
| outra ou nenhuma | Outros | `#26a269` | pasta |

A tabela fica em `desktop_group()` e `groups[]`, em `src/ui/core/desktop.c`. Os nomes
dos grupos estão traduzidos (pt, en, es) em `i18n_strings.def` (`S_GRP_*`).

## Apps instalados × comandos

| Onde está o `.desktop` | O que é | Aparece em |
|---|---|---|
| `<dados>/apps/<id>/<nome>.desktop` | app instalado (pacote) | grupo na grade de Apps **e** Run |
| `<dados>/apps/<nome>.desktop` ou `/usr/share/bigfoston/apps/` | comando | só no Run |

`<dados>` é `/data/bigfoston`, no cartão SD. Os apps instalados sobrevivem a
desligar e atualizar o sistema.

## Ícone

`Icon=icon.svg` é relativo à pasta do app e não aceita `..` nem caminho absoluto. O SVG é
rasterizado uma vez em 56×56, como **máscara A8** (`svg_render_mask`, `core/svgimg.c`):
- vale só a forma (a opacidade de cada pixel), que o shell pinta de branco sobre o degradê
  da cor do app, como os ícones do sistema;
- assim um app de fora fica com a mesma cara dos nativos, nos temas claro e escuro.

O ícone fica guardado até a próxima instalação ou desinstalação. Sem ícone, o app mostra a
inicial do nome.
