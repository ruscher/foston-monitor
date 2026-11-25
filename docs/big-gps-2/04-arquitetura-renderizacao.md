# Arquitetura: GPS, movimento, câmera e quadros em ritmos separados

```
GPS (1 Hz) ──► on_fix ──► bgn_update (casa na rota, manobras, voz)
                    └──► bgmo_fix (biggps/bgmotion.c)
                                │
anim_cb (20 Hz) ──► bgmo_step ──► posição animada + rumo do carro + rumo da câmera
                                │
                                ├──► cur (centro/rotação pedidos) ──► update_offset: desliza o quadro pronto
                                │                                      (só redesenha se mudou 1 px)
                                └──► precisa de quadro novo? ──► request() ──► thread do motor (nice 10)
                                     (≥16 px fora, ≥2° de giro,           quadro pronto ──► tick_cb troca o buffer
                                      outro zoom, ou 1 s descentrado,
                                      e o motor livre)
```

| Etapa | Ritmo | Onde |
|---|---|---|
| Leitura do GPS | 1 Hz (o receptor) | `tick_cb` → `gps_update` |
| Rota, manobras, voz | a cada posição | `on_fix` → `bgn_update`, `announce` |
| Posição animada e câmera | 20 Hz (`ANIM_MS` 50) | `anim_cb` → `bgmo_step` |
| Deslizar a imagem | quando o deslocamento muda 1 px | `update_offset` |
| Quadro novo do mapa | sob demanda, motor livre | `anim_step` → `request` |
| Marcador | a cada passo (só a área dele) | `update_marker` |

## Decisões

- **O mapa-base não é redesenhado a cada posição.** A imagem pronta é deslocada em memória pelo
  LVGL. O motor só desenha de novo quando a imagem deslocada já não basta: borda exposta de
  16 px, 2° de giro acumulado, outro zoom, ou 1 s fora do centro. Assim o carro anda pixel a
  pixel enquanto o motor trabalha em segundo plano.
- **Giro pequeno também desliza.** `update_offset` aceita até 12° de diferença entre o quadro
  na tela e a câmera. Desloca pelo centro, e o quadro seguinte acerta o giro. Antes, qualquer
  diferença de giro congelava a imagem.
- **Nada de redesenho à toa.** A 50 km/h e zoom 16, o carro anda ~6 px/s, menos de 1 px por
  passo de 50 ms. O passo só invalida a tela quando o deslocamento inteiro muda (ou chega
  quadro novo). Sem isso o LVGL redesenhava 800×480 a 15 Hz com a mesma imagem: 85 % de CPU, e
  o motor ficava sem tempo (quadros de 575 ms).
- **O motor continua com prioridade menor** (`nice 10`, já existia): toque e animação ficam à
  frente do desenho do mapa.

## Limites conhecidos (próximas fases)

- O giro da câmera aparece em degraus de até ~2–6°, a cada quadro novo do motor (~100–200 ms
  em navegação). Girar a imagem pronta a cada passo custaria uma transformação de
  800×480 por software (fase 3/5, a medir).
- A troca de zoom pela velocidade (16,5 → 16 → 15,5) ainda é instantânea: espera o quadro
  novo, sem escalar a imagem anterior.
- As bordas expostas no deslocamento são pintadas com a cor de fundo até o quadro novo chegar.
  Desenhar o mapa com margem resolveria, ao custo de mais área por quadro.
