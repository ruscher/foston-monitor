# Vídeo: a imagem do computador no FOSTON

`src/remoteplay/video.c`. O H.264 do Sunshine é decodificado com o **mesmo FFmpeg mínimo da TV**
(`firmware/build/ffmpeg-tv`, estático; nenhum FFmpeg a mais) e desenhado direto no framebuffer em
RGB565, numa caixa de 640×360 que o shell deixa preta, como a TV em janela.

## Escolhas para o ARM11

| Escolha | Por quê |
|---|---|
| `AV_CODEC_FLAG_LOW_DELAY`, uma thread | sem fila de reordenação: cada quadro sai assim que chega |
| sem o filtro de desblocagem (`skip_loop_filter`) | −30 % de CPU no ARM11 (medido na TV) |
| fila e descarte da moonlight-common-c | se a decodificação atrasa, a fila transborda e um quadro-chave novo é pedido ("Video decode unit queue overflow" → "IDR frame request") |
| erro de decodificação → `DR_NEED_IDR` | a imagem se recupera sozinha |
| ampliação 2× com escrita de 32 bits; outros tamanhos, vizinho mais próximo | as tabelas de cor do `bftvd` |
| `SHOW 0` quando há diálogo ou aviso por cima, ou na vista do controle | o vídeo não apaga a interface |

O motor manda `VSTATS qps erros ms` a cada 2 s; a tela mostra os quadros por segundo.

## Medido no aparelho (24/11/2026)

Rede USB, entrada "Desktop Low" do Big Remote Play (o desktop com dois monitores), H.264 do
Sunshine 7.1.431, vídeo ampliado para 640×360.

| Pedido ao Sunshine | Quadros/s mostrados | ms por quadro | Erros | CPU do motor |
|---|---|---|---|---|
| 320×180, 30 qps, 1000 kbit/s | **22–29** (vales de 8 por ~2 s) | 34–41 | 0 | 38 % (direto) / 71 % (no app, com a interface) |
| 480×270, 20 qps, 1500 kbit/s | **8–11** | 89–119 | 0 | 71 % |
| 640×360, 15 qps, 2000 kbit/s | **5–6** | 150–195 | 0 | 43 % (limitado pela decodificação) |

Daí as três opções da tela, gravadas em `<dados>/remoteplay/quality`:
- **Fluido** (320×180): o padrão, para jogos;
- **Nítido** (480×270);
- **Máximo** (640×360): para ler a tela e usar o mouse.

O texto pequeno do desktop fica ilegível em Fluido.

## Pendente

- Tela cheia (800×450, como a TV);
- medir o atraso do toque até a imagem;
- 400×240 e 10 qps, que o pedido original sugeria (é fácil acrescentar à tabela, falta medir).
