# Mouse pela imagem (touch direto)

Na vista **Com imagem**, a caixa do vídeo recebe o toque (`on_video_touch` em `scr_brp.c`).

| Gesto | O que o computador recebe |
|---|---|
| encostar / arrastar | o ponteiro na posição tocada (`LiSendMousePositionEvent`, relativo à caixa 640×360) |
| toque curto (< 14 px de movimento) | clique esquerdo |
| toque longo parado (> 0,6 s) | clique direito |
| "Botão direito" ligado e um toque | clique direito (desarma sozinho) |
| "Arrastar" ligado | botão esquerdo apertado enquanto o dedo anda (arrastar e soltar) |
| "Rolar" ▲ / ▼ | rolagem de 2 cliques |

Gestos de dois ou três dedos não existem: o painel tem um ponto só
([11](11-touchscreen-analysis.md)). Por isso viraram botões.

## Conferido no aparelho

Um toque perto do canto superior esquerdo e outro perto do canto inferior direito da imagem fizeram
o realce do ponteiro aparecer **nos mesmos lugares** na imagem transmitida. O mapeamento está
certo mesmo com as faixas pretas que o Sunshine põe (o desktop de dois monitores não é 16:9).

O `xdotool` não serve para conferir: a sessão do computador é Wayland.

## Pendente

Modo touchpad (movimento relativo), clique do meio e rolagem horizontal.
