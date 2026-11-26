# Zoom inteligente nas manobras

`bgn_zoom256` (`src/ui/biggps/bgnav.c`) decide o zoom da câmera na navegação a cada posição:

| Situação | Zoom |
|---|---|
| Até 50 km/h | 16,5 |
| 50–80 km/h | 16 |
| Acima de 80 km/h | 15,5 |
| Curva comum a menos de max(120 m, 6 s na velocidade atual) | 17 (ou o da velocidade, se maior) |
| **Manobra complexa** (rotatória, acesso/saída, mantenha-se, retorno, curva acentuada) a menos de max(200 m, 9 s) | **17,5** |
| Seguir em frente, partida, chegada | sem aproximação |
| Bicicleta e a pé | 17 sempre |

- Depois da manobra, a próxima define o zoom, e ele volta sozinho ao da velocidade.
- A troca anda ~0,1 nível por quadro do motor (`zoom_goal`), sem saltos.
- Um zoom feito à mão vale até a situação mudar (outra faixa de velocidade ou outra manobra).
- Arrastar o mapa desliga o acompanhamento e o zoom automático.

O momento depende da velocidade, da distância e do tipo da manobra. A complexidade real do
cruzamento (quantas vias saem dele) ainda não entra: o pacote de rotas não guarda isso.

Testes: `tests/test_bgnavzoom.c`, com 12 casos (velocidades, curva comum e complexa nos limites
de distância, chegada, bicicleta e sem rota).
