# Grupos de apps — arquitetura

Os grupos são **do sistema, não do tema**. Há uma só organização dos aplicativos, gravada num
arquivo; todas as telas que mostram apps leem dela e só cuidam do desenho. Trocar de tela
inicial em Ajustes > Aparência não muda nada na organização.

## Camadas

```
core/appgroups.c   modelo + gravação  (C puro, sem LVGL; testado em tests/test_appgroups.c)
       │
core/launcher.c    ponte: ID estável → nome, ícone, cor, o que abrir; acha apps novos
       │
screens/launcher_ui.c   fluxos de interface iguais em todos os temas:
       │                menu do app, menu da pasta, criar, enviar, renomear, cor e ícone,
       │                ordenar, excluir, modo mover, busca, avisos
       │
widgets/hold.c     toque longo de 2 s com anel de progresso
widgets/ctxmenu.c  menu de contexto e lista de escolha
widgets/namebox.c  janela de nome com o teclado pt-BR (acentos)
       │
scr_home.c (Grade, Apps do Glass, Apps do Painel)   scr_appgroup.c (tela de um grupo)
```

As telas chamam só duas funções: `lui_bind_tile()` em cada ícone (toque abre, 2 s abre o
menu, no modo mover o toque coloca o app ali) e `lui_move_bar()` para a faixa do modo mover.

## Onde cada tema mostra os grupos

| Tela inicial | Onde ficam os apps e grupos | Observação |
|---|---|---|
| Grade | a própria tela inicial | mesma grade da tela Apps |
| Painel | ícone **Apps** novo, no canto direito da fileira de atalhos | abre a grade (`SCR_HOME`, `HOME_ARG_APPS`) |
| Foston Glass | botão Apps do dock (já existia) | abre a mesma grade |
| Quiosque | não mostra grupos nem menu | de propósito: o quiosque é travado nos apps escolhidos |

Os atalhos do Painel e os apps do Quiosque continuam vindo de `home_apps` (Ajustes >
Aparência > Apps escolhidos): são favoritos, independentes dos grupos. Pôr um app num grupo
não tira o atalho dele do Painel.

## IDs estáveis

Nunca o nome exibido (que muda com a língua e pode repetir):

| Tipo | ID | De onde vem |
|---|---|---|
| app do sistema | `sys:<chave>` | coluna `key` da tabela de `scr_home.c` (`music`, `tv`, `settings`...). **Nunca mudar uma chave.** |
| pacote `.bfapp` | `pkg:<id>` | nome da pasta em `<dados>/apps/<id>/` |
| WebApp | `web:<url>` | a URL (única em `webapps.tsv`) |
| pasta de grupo | `grp:<n>` | número do grupo, nunca reaproveitado |

## Modelo

Uma lista ordenada de entradas `{id, pai}`. Pai 0 é o nível principal (a tela Apps); pai
`n > 0` é o grupo `n`. A ordem dentro de um pai é a ordem relativa na lista. A pasta de um
grupo é a entrada `grp:<n>` no nível principal: a posição dela é a posição da pasta na grade.

Grupos: `{id, key, name, color, icon}`.
- `key` = categoria de origem (`games`, `media`...) ou vazio (criado pelo usuário).
- `name` vazio = o nome da categoria traduzido na hora (Jogos/Games/Juegos). Renomear grava
  um nome próprio; a `key` fica, e os jogos instalados depois continuam indo para lá.

Regras garantidas pelo núcleo: sem duplicatas (um id aparece uma vez), sem pasta dentro de
pasta, nome de grupo único (sem diferença de maiúsculas e acentos maiúsculos), grupos vazios
permitidos, IDs de grupo crescentes.

## Apps novos, removidos e a migração

`launcher_sync()` (ao abrir a grade, a tela de um grupo e depois de instalar) entrega ao
núcleo a lista do que existe agora:

- app do sistema novo → fim do nível principal;
- pacote novo → grupo da categoria dele (o `Categories=` do `.desktop`), criado no fim se
  ainda não existir — o mesmo comportamento de antes ("o app entra sozinho no grupo certo");
- WebApp novo → fim do nível principal;
- **semente** "games": só na primeira vez (sem arquivo), cria Jogos mesmo vazio, como antes.

O que **sumiu não é apagado** (cartão fora, pacote ilegível): só fica escondido e volta ao
mesmo lugar. Apagar de vez só quando o usuário desinstala o pacote (`apps_uninstall_now` →
`launcher_forget_pkg`) ou remove o WebApp (`launcher_forget_web`).

**Migração** das versões anteriores: não havia arquivo; a primeira sincronização monta a
organização na ordem que a grade antiga mostrava (sistema, Jogos e os grupos de categoria com
os pacotes, WebApps). Conferido no aparelho real: os 2 pacotes instalados foram para Jogos e
os 2 WebApps ficaram no fim, sem perder nada.

## Atualização da interface

Toda mudança feita pelo usuário passa pelo núcleo, que grava e avisa o ouvinte
(`ag_set_listener`). `launcher_ui.c` agenda uma reconstrução assíncrona (`lv_async_call`) da
tela aberta, se for a grade ou um grupo; se o grupo aberto foi excluído, volta. A
sincronização não avisa (quem sincroniza já está desenhando). `ag_serial()` sobe a cada
mudança: o preenchimento em segundo plano da grade para se a organização mudar no meio.

## Desempenho

- Grade: a página à vista nasce na hora; as outras são preenchidas por um temporizador de
  15 ms, 2 ícones por vez, vizinhas primeiro; saltar para uma página ainda vazia (setas,
  controle, FIFO) completa a página na hora. Medido no FOSTON com 41 pastas e 7 páginas,
  abrindo Apps a partir da tela inicial Glass: o laço mais longo caiu de 838 ms (tudo de uma
  vez) para 447 ms (esse número inclui desmontar a tela Glass); vindo de outra tela, 175–210
  ms — o shell original leva 211 ms com só 20 itens. Enquanto o resto é preenchido, o laço
  fica em ≤ 42 ms.
- O toque longo não usa temporizador: a contagem anda nos eventos `PRESSING` do LVGL. O anel
  só aparece depois de 250 ms e só se redesenha quando o valor muda.
- Gravar = um arquivo de texto pequeno (centenas de bytes), só quando algo muda.
- Memória: a organização inteira são algumas centenas de bytes por app; nada fica em cache
  além disso.

## Arquivos

Novos: `core/appgroups.{c,h}`, `core/launcher.{c,h}`, `screens/launcher_ui.{c,h}`,
`widgets/hold.{c,h}`, `widgets/ctxmenu.{c,h}`, `widgets/namebox.{c,h}`,
`tests/test_appgroups.c`.

Alterados: `screens/scr_home.c` (chaves estáveis, grade pela organização, preenchimento aos
poucos, Apps no Painel, lupa), `screens/scr_appgroup.c` (refeito: argumento = id do grupo,
botão Opções), `screens/screens.h`, `core/apps.{c,h}` (desinstalar tira dos grupos; o aviso
diz o grupo real), `core/ctl.c` (`group`, `menu`, `pick`), `core/hwkbd.c` (Esc fecha o menu),
`core/app.c` (`lui_init`), `core/i18n_strings.def` (`S_LG_*` em pt, en, es),
`widgets/tile.c` (miniaturas da pasta), `core/screenshot.c` (testes), `Makefile` (teste novo).

Ver também: [UX/UI](launcher-groups-ux-ui.md), [persistência](launcher-groups-persistence.md),
[testes](launcher-groups-tests.md), [relatório final](launcher-groups-final-report.md).
