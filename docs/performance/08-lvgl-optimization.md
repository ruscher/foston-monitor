# LVGL e framebuffer

Configuração conferida (`src/ui/lv_conf.h`, gerado por `tools/make_lv_conf.py`): LVGL
9.2.2, renderização por software, `LV_DRAW_SW_COMPLEX` 1, `LV_STDLIB_CLIB`,
`LV_DEF_REFR_PERIOD` 33 ms, fbdev parcial com 2 buffers.

## O que pesava (perfil e medidas no aparelho)

1. **A tela inteira desenhada duas vezes.** O mapa (e os visualizadores em tela cheia)
   ficam num objeto opaco em `lv_layer_top`; o LVGL 9.2 sempre redesenha a tela ativa
   por baixo antes da camada superior. Patch `src/ui/patches/0005`: se um objeto da
   camada superior cobre a área com opacidade total, desenha a partir dele e pula as
   telas. Arrasto: 7,7 → 12 quadros/s.
2. **Rótulos do mapa.** Cada nome era desenhado 9 vezes pelo LVGL (8 de contorno + texto)
   em todo quadro, e os nomes de vias girados eram imagens ARGB transformadas a cada
   quadro. Agora cada rótulo vira um bitmap com contorno (cache de 64 entre quadros) e é
   misturado no buffer RGB565 do mapa uma vez por quadro do motor (`compose_labels`,
   rotação por mapeamento inverso bilinear em ponto fixo).
3. **Faixas de 48 linhas.** O buffer parcial de 48 linhas obrigava 10 passes pela árvore
   de objetos por quadro; agora o buffer tem a tela inteira (2 × 768 KB). Junto, o cache
   de sombras (`LV_DRAW_SW_SHADOW_CACHE_SIZE` 32, 1 KB): os botões redondos recalculavam
   a sombra a cada quadro. 12 → 16,5 quadros/s.
4. **Fundo sob a imagem.** O fundo opaco de 800×480 era pintado e logo coberto pela
   imagem do mapa (22 % do tempo). Agora `ov_draw` pinta só as faixas descobertas no
   arrasto e `ov_cover` informa ao LVGL que a tela fica coberta. Controles do mapa opacos
   (a mistura por pixel custava ~6 %).

## Piso atual

Perfil depois das mudanças: ~50 % `memcpy` (imagem do mapa → buffer, buffer → framebuffer).
Com 58 MB/s de banda, cada cópia de tela inteira custa ~13 ms. Desenhar direto no
framebuffer evitaria uma cópia, mas a memória do framebuffer é *write-combined* (ler dela
para misturar é lento) e com um buffer só o usuário veria o quadro sendo montado;
ficou de fora ([19](19-known-limitations.md)).

## Interface parada

O laço principal espera com `poll()` no evdev do toque e do botão de energia
(`main.c: wait_events`) em vez de ler o toque a cada 33 ms e o botão a cada 40 ms; o toque
usa `LV_INDEV_MODE_EVENT` (com o dedo na tela o LVGL continua lendo pelo próprio timer).
57 → 4 acordadas/s.
