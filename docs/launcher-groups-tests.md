# Grupos de apps — testes

Três níveis, do mais rápido ao mais real. Só o terceiro comprova o funcionamento no
aparelho; os outros dois pegam regressões a cada mudança.

## 1. Unidade: o núcleo (`make test`)

`src/ui/tests/test_appgroups.c` testa `core/appgroups.c` sozinho (sem LVGL), compilado com
AddressSanitizer e UBSan: **64/64**.

Cobre: migração (Jogos como semente, pacote no grupo da categoria, ordem antiga), criar
grupo levando o app (pasta no lugar dele), nomes (vazio, só espaços, repetido com
maiúsculas, repetido com o nome traduzido de uma categoria, acentos, 24 × 25 caracteres
UTF-8, TAB/quebra de linha), enviar, enviar de novo (sem duplicar), mover entre grupos, tirar
do grupo, pasta dentro de pasta (recusado), reordenar, ordenar, renomear (inclusive grupo de
categoria, que mantém a key), cor e ícone, reabrir o arquivo (mesma organização), excluir
(apps voltam no lugar da pasta, na ordem), ids não reaproveitados, app que some e volta,
desinstalar/reinstalar, **erro de gravação** (falha injetada: nada muda), arquivo truncado
(usa `.bak` e guarda `.bad`), versão futura, dados inválidos e repetidos, 200 apps + 48
grupos sem perder nenhum.

```
cd src/ui && make test
```

## 2. Interface no simulador, com toque de verdade

`core/screenshot.c`, função `group_tests()`: cria um dispositivo de ponteiro do LVGL que
lê um "dedo" virtual. Os toques passam pelo mesmo caminho do aparelho (PRESSED, PRESSING,
rolagem, RELEASED, CLICKED), com limite de rolagem de 16 px como o toque calibrado.
**61/61**, também no build com ASan/UBSan, sem erro de memória.

```
cd src/ui && make sim
BIGFOSTON_TEST_GROUPS_ONLY=1 BIGFOSTON_DATA=/tmp/dados ./build/sim/bigfoston-shell --screenshot /tmp/shots
# ou a suíte inteira (todas as telas + teclado + controle + pacotes + grupos):
BIGFOSTON_TEST_BFAPP=$PWD/../../pkg/apps/sc-controller.bfapp make screenshots
```

A suíte inteira terminou com teclado 0 falhas, controle 0, pacotes 0, grupos 61/61 e 184
capturas; `screenshot_run` agora devolve erro se algum teste de grupo falhar.

## 3. No FOSTON real (25/11/2026)

Shell compilado pelo Buildroot (`make O=../out bigfoston-shell`, ARMv6 sem VFP, sem avisos)
e rodado de `/tmp` com os dados reais (`/data/bigfoston`), sem instalar. Toques escritos no
`/dev/input/event0` (o nó do driver do touchscreen) com a calibração inversa do `pointercal`,
capturas lidas do `/dev/fb0`. No fim o shell instalado voltou, a tela inicial voltou para
Grade e o `appgroups.conf` de teste foi apagado.

Limite honesto: o toque injetado entra no núcleo de entrada do Linux pelo mesmo caminho do
dedo, mas não reproduz o tremor de um dedo de verdade no resistivo. A folga de 18 px e o
limite de 0,7 s foram escolhidos para isso, mas valem uma conferência com a mão.

## Os 25 cenários pedidos

| # | Cenário | Unidade | Simulador | Aparelho |
|---|---|---|---|---|
| 1 | toque rápido abre o app | — | ✓ TV | ✓ pasta, Apps do Painel |
| 2 | 2 s abre o menu (sem abrir o app) | — | ✓ (1,1 s ainda sem menu) | ✓ |
| 3 | criar grupo leva o app | ✓ | ✓ | ✓ "Diversão" com a TV, "Util" com Arquivos |
| 4 | enviar para grupo existente | ✓ | ✓ | ✓ TV → Diversão, Multimídia → Util |
| 5 | mover entre dois grupos | ✓ | ✓ | não feito |
| 6 | remover do grupo | ✓ | ✓ | ✓ |
| 7 | renomear | ✓ | ✓ (título da tela muda) | não feito |
| 8 | excluir sem desinstalar | ✓ | ✓ | ✓ (TV voltou ao lugar) |
| 9 | nomes com acento | ✓ | ✓ "Educação" | ✓ "Diversão" pelo teclado da tela (áã) |
| 10 | nome vazio | ✓ | ✓ (janela continua aberta) | — |
| 11 | nome repetido | ✓ | ✓ ("jogos" = Jogos) | — |
| 12 | vários grupos | ✓ 48 | ✓ 40 | ✓ 41 (arquivo de carga) |
| 13 | trocar o tema mantém os grupos | — | ✓ Glass, Painel, Quiosque, Grade | ✓ Grade → Painel → Glass |
| 14 | reiniciar mantém | ✓ reabrir | ✓ reler o arquivo | ✓ processo reiniciado; **desligar e ligar não foi feito** (reinício remoto bloqueado nesta sessão) |
| 15 | Apps no Painel | — | ✓ | ✓ |
| 16 | grupos no Foston Glass | — | ✓ | ✓ menu da pasta, novo grupo em vidro |
| 17 | outros temas | — | ✓ Quiosque (Apps abre a grade sem Ajustes, sem menus, pasta sem Opções, busca sem Ajustes), tema claro | — |
| 18 | rolar sem abrir menu | — | ✓ arrastar 120 px | ✓ arrastar 150 px |
| 19 | cancelar o toque longo | — | ✓ soltar com 1 s | só o arrastar |
| 20 | resistivo de um ponto | — | — | ✓ (toque injetado no driver, ver limite acima) |
| 21 | mouse e teclado | — | ✓ teclado físico: Esc fecha menu e janela, Enter cria, digitar | mouse não testado |
| 22 | nomes longos | ✓ 24/25 | ✓ campo para em 24; reticências na grade | — |
| 23 | instalar e remover apps | ✓ | ✓ .bfapp vai para Jogos; desinstalar tira dos grupos | migração com 2 pacotes reais ✓ |
| 24 | erro de gravação | ✓ falha injetada | — | — |
| 25 | desempenho | ✓ 48 grupos + 150 envios gravando: 69 ms no PC | ✓ só a página à vista | ✓ números abaixo |

## Desempenho medido no aparelho (`perfstat` do FIFO de controle)

| Medida | Resultado |
|---|---|
| abrir Apps (20 itens) — shell original | laço máx. 211 ms |
| abrir Apps — shell novo, vindo de outra tela, 41 pastas | 175–210 ms |
| abrir Apps vindo do Glass, 41 pastas, tudo de uma vez (1ª versão) | 838 ms |
| o mesmo com o preenchimento aos poucos | 447 ms (inclui desmontar a tela Glass) |
| enquanto as outras 6 páginas são preenchidas | laço máx. 42 ms |
| pular para a 7ª página | laço máx. 29 ms |
| abrir um grupo | laço máx. 99 ms |

## Defeitos achados e corrigidos durante os testes

- **Miniaturas da pasta** (já existia): com a borda de 1 px o quadrado ficava com 64 px por
  dentro e 2 × 30 + 6 não cabiam: só a coluna da esquerda aparecia (um grupo com 3 apps
  mostrava 2). Espaçamento refeito (79 px).
- **Aviso discreto mostrava "…"** no aparelho: rótulo com reticências dentro de um
  contêiner de largura automática encolhia a zero. Largura calculada pelo texto.
- **Ponteiro pendente no aviso**: reconstruir a interface apagava o aviso por fora e a
  próxima confirmação usava o objeto apagado (falha de segmentação no simulador). Zerado no
  evento DELETE.
- **Grade lenta com muitas pastas**: 838 ms num laço; agora a página à vista primeiro.
- **Reconstrução dupla**: a sincronização avisava o ouvinte e a grade era refeita logo
  depois de criada; tirado.
