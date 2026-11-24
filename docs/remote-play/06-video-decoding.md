# Vídeo: a imagem do computador no FOSTON

`src/remoteplay/video.c`. O H.264 do Sunshine é decodificado com o **mesmo FFmpeg mínimo da TV**
(`firmware/build/ffmpeg-tv`, estático; nenhum FFmpeg a mais) e desenhado direto no framebuffer em
RGB565, numa caixa de 640×360 que o shell deixa preta, como a TV em janela.

## Escolhas para o ARM11

| Escolha | Por quê |
|---|---|
| `AV_CODEC_FLAG_LOW_DELAY`, uma thread | sem fila de reordenação: cada quadro sai assim que chega |
| **filtro de desblocagem ligado** | sem ele os blocos do H.264 aparecem ("quadriculado"); custa só ~5 ms por quadro em 320×180 |
| **ampliação suave** | ver abaixo; antes era vizinho mais próximo (cada ponto virava um quadrado 2×2) |
| fila e descarte da moonlight-common-c | se a decodificação atrasa, a fila transborda e um quadro-chave novo é pedido |
| erro de decodificação → `DR_NEED_IDR` | a imagem se recupera sozinha |
| `SHOW 0` com diálogo ou aviso por cima e na vista do controle; `CLIP n` com a barra da tela cheia à mostra | o vídeo não apaga a interface |
| motor com nice 5 | decodificando, ele ocupava a CPU e o toque e a barra chegavam segundos atrasados |

### Ampliação suave e barata

Uma bilinear calculada por ponto de saída custou ~85 ms por quadro em 800×450 (28 → 7 qps).
A versão final:
1. converte a cor **uma vez por ponto de origem**;
2. amplia cada linha de origem na horizontal uma vez (guarda as duas últimas);
3. usa como meio-termo a **média no próprio RGB565** (`((a & 0xF7DE) + (b & 0xF7DE)) >> 1`), sem
   multiplicação;
4. escreve dois pontos por vez (32 bits) no framebuffer, que não tem cache.

### FFmpeg "rápido": testado e descartado

Mesmo decodificador sem `--enable-small` e com `-O2`, medido no aparelho com gravações de TV:

| Gravação | `--enable-small` (o da TV) | `-O2` |
|---|---|---|
| Globo (Baseline), sem / com desblocagem | 47,4 / 34,6 qps | 44,2 / 32,5 qps |
| SBT (Main, CABAC), sem / com desblocagem | 27,5 / 23,2 qps | 28,0 / 22,8 qps |

Não há ganho (no ARM11, código menor cabe melhor no cache). Fica o da TV.

## Medido no aparelho (24/11/2026)

Rede USB, entrada "Desktop Low" (o desktop com dois monitores), H.264 do Sunshine 7.1.431, com o
filtro de desblocagem e a ampliação suave.

| Qualidade | Pedido ao Sunshine | Tela cheia (800×450) | Janela (682×384) |
|---|---|---|---|
| **Alta** | 640×360, 15 qps, 3000 kbit/s | ~5 qps | — |
| **Média** | 480×270, 20 qps, 2000 kbit/s | ~9 qps | — |
| **Baixa** | 320×180, 30 qps, 1000 kbit/s | ~19 qps (18,5–20) | 15 qps (medido no app) |

0 erros de decodificação em todas as medições.

Para ler textos, prefira uma entrada de um monitor só ("01 - Monitor"): a "Desktop Low" junta os
dois monitores numa imagem só e cada ponto do FOSTON cobre muitos pontos do desktop.

## Sem faixas pretas, sem esticar, com zoom

- **Faixas pretas de dentro do vídeo:** o Sunshine as põe quando a tela do computador não tem a
  proporção pedida (o desktop com dois monitores, por exemplo). A cada 15 quadros o motor varre
  as bordas e deixa de fora as linhas e colunas pretas (brilho ≤ 24 em toda a amostra).
- **Cobrir a área:** o que sobra é ampliado **cobrindo** a área de exibição, sem deformar. O excesso
  das bordas fica de fora, e a janela usa a largura toda.
- **Lupa − / +:** zoom de 100, 150, 200, 300 e 400 %. Com zoom, arrastar o dedo **move a imagem**
  (`PAN`) e o toque curto continua clicando.
- **Toque:** o motor converte cada toque em posição no vídeo pelo mesmo recorte (`video_map`), então
  o clique cai no lugar certo com corte e com zoom.

Conferido no aparelho: a janela ficou sem faixas, e em 200 % o arrasto levou a vista para outra
parte do desktop.

## Tela cheia

- **Área:** o vídeo em 800×450 numa camada por cima de tudo.
- **Toque na imagem:** clica no PC e faz a barra pequena **subir de baixo** por 4 s. Medido no
  aparelho pelo horário do framebuffer: a barra aparece e desce depois de 4 s.
- **Linhas cobertas:** enquanto a barra aparece, o motor não pinta as linhas que ela cobre
  (`CLIP`).
- **Janela:** volta pelo botão "Janela" da barra.

## Pendente

Medir o atraso do toque até a imagem; som.
