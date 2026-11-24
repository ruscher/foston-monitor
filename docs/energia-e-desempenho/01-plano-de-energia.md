# Etapa 1 — Plano de energia (Ajustes > Potência)

O FOSTON não tem `cpufreq`: a CPU fica sempre a 468 MHz. Por isso o plano não mexe no
processador. Ele age no que gasta CPU e bateria e que dá para controlar: o brilho, o escurecer,
os sons, os efeitos, o que roda em segundo plano e o detalhe do mapa.

## Os três planos

| | Economia | Equilibrado (padrão) | Desempenho |
|---|---|---|---|
| Brilho | até 50 % | como está | como está |
| Escurecer a tela | 1 min | nunca | nunca |
| Sons de toque | não | sim | sim |
| Efeitos visuais | mínimos | completos | mínimos |
| Modo performance | sempre (GPS e tarefas pausados) | com vídeo, TV ou controle | sempre e com vídeo, TV ou controle |
| Mapa do Big GPS | econômico | automático | econômico |

- **Um toque grava todos os ajustes** (`powerplan_apply`). Depois cada um pode ser mudado à mão e
  o plano continua marcado.
- Os motivos automáticos (vídeo, TV, controle) ficam ligados nos três planos.
- O Equilibrado repõe exatamente os valores de fábrica: escolher ele desfaz os outros.

## Por que "sempre ligado" pausa só GPS e tarefas

Com o motivo `PERF_WHY_PLAN` (o plano, sem vídeo, TV nem controle), o `perfui` pausa só
`PM_SVC_GPS | PM_SVC_CRON`:
- o **Bluetooth** continua, porque o celular precisa conectar e a música tocar;
- o **telefone** continua, porque as chamadas precisam chegar.

Um app que precisa de um serviço o religa enquanto está aberto: Mapa e GPS religam o GPS.

O `bfgps` é o maior consumidor com o aparelho parado. Ele roda o `gps.dll` do WinCE, que faz o
processamento do sinal na CPU:

| Processo (parado na Início, 20 s) | CPU |
|---|---|
| `bfgps` | **9,9 %** |
| `bigfoston-shell` | 3,3 % |
| todo o resto | < 1 % cada |

O Equilibrado deixa o GPS ligado para a posição estar pronta ao abrir o Mapa. Economia e
Desempenho trocam isso por metade da CPU parada:

| Plano | CPU ocupada parado na Início |
|---|---|
| Equilibrado | 15,7 % |
| Desempenho | **8,2 %** |

## Efeitos visuais (Ajustes > Desempenho)

| Nível | Animações | Papel de parede |
|---|---|---|
| Completos | sim | sim |
| Reduzidos | não | sim |
| Mínimos | não | não: fundo liso na cor do tema, menos a desenhar em cada quadro |

O fundo liso usa `app_wallpaper_plain`, separado do fundo preto da TV (`app_wallpaper_visible`).
As duas coisas podem valer ao mesmo tempo sem uma desfazer a outra.

## As telas

- **Potência:**
  - a bateria em cima;
  - "Plano de energia" com três cartões (`page_choice`, ✓ no escolhido);
  - a nota: "O processador do FOSTON tem frequência fixa: o plano age no brilho, nos efeitos e
    no que roda em segundo plano";
  - Desligar.
- **Desempenho:**
  - um cartão com o plano atual e os atalhos **Potência** e **Modo performance**;
  - "Efeitos visuais" (três cartões);
  - "Detalhe do mapa" (os quatro de antes, agora no mesmo formato).

Pelo FIFO de controle: `plan 0|1|2` e `fx 0|1|2`. Se a tela do plano estiver aberta, ela se
refaz.
