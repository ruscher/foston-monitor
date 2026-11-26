# Posição entre duas leituras do GPS (`biggps/bgmotion.c`)

O receptor dá ~1 posição por segundo. `bgmotion` mantém uma **posição de exibição** que anda
até a posição estimada, em vez de saltar. Não tem LVGL nem relógio próprio: quem chama passa o
tempo em µs, e os testes usam trajetos sintéticos.

## Na rota (navegando e casado na via)

A posição é uma **distância ao longo da rota** (`s`, em decímetros), e o ponto na tela sai da
geometria da rota (`bgmo_route_point`). Por construção o carro **não corta esquinas nem
atravessa quarteirões**.

- Alvo: `s_fix + v × t_desde_o_fix`, limitado a **1,5 s** e **45 m** de previsão
  (`BGMO_PRED_US`, `BGMO_PRED_M`) e ao fim da rota.
- A cada passo: avança `v × dt` (enquanto há previsão) e corrige o erro com um filtro de
  primeira ordem (`TAU_POS` = 0,9 s). Velocidade constante dá erro zero; o ruído do GPS vira
  uma leve aceleração ou desaceleração, não um salto.
- **Nunca anda para trás** por causa de ruído: se a posição real ficou até 15 m atrás, o carro
  espera. Mais que 15 m é correção de verdade. Mais que 80 m é salto (rota nova, recálculo).
- O rumo do carro é o da rota em ±6 m em volta do ponto: esquinas viram em arco.

## Fora da rota (sem navegação ou fora da via)

Previsão pelo rumo e pela velocidade (mesmos limites de tempo e distância), com o mesmo filtro.
**Parado** (abaixo de 3 km/h), a posição só muda se o GPS se afastar mais de 6 m: o ruído do
receptor não faz o carro "passear".

## Sem sinal

Depois de 1,5 s sem posição nova o carro para no último ponto previsto. Depois de 3 s
`stale` = 1 e o marcador fica cinza (o mesmo ícone de "sem GPS"). **O carro nunca segue
dirigindo sozinho.**

## Câmera

Segue o rumo do carro com suavização própria, mais lenta (`TAU_CAM` 0,7 s, no máximo 90°/s),
sempre pelo **menor ângulo** (`bgmo_angle_diff`): de 350° para 10° passa por 0°, nunca por 180°.

## Testes (`make test` em `src/ui`, com ASan/UBSan)

[`tests/test_bgmotion.c`](../../src/ui/tests/test_bgmotion.c), resultado de 25/11/2026:

| Cenário | Resultado |
|---|---|
| Reta a 50 km/h | passo máx. 0,70 m por 50 ms (esperado 0,69), atraso 0,04 m |
| Reta a 110 km/h | passo máx. 1,53 m (esperado 1,53), atraso 0,04 m |
| Duas esquinas de 90° a 40 km/h | fora da via 0,000 m, câmera ≤ 3,3° por passo |
| Retorno (180°) a 20 km/h | fora da via 0,000 m, câmera ≤ 3,1° por passo |
| Esquinas a 50 km/h, GPS com ±5 m e até 300 ms de atraso | sem ré, passo máx. 1,17 m, fora da via 0 |
| Parado com ruído de ±4 m por 30 s | marcador a no máx. 3,3 m do ponto inicial |
| Sem sinal por 15 s a 72 km/h | parou 30 m depois do último ponto (limite 45), marcado incerto |
| Rumo 350° → 10° | maior afastamento do norte: 10° (o lado curto) |
| Ponto 500 m adiante (rota nova) | salta, não desliza |

Escolha do `TAU_POS` (cenário com ruído): 0,35 s → passo máx. 1,92 m; 0,6 → 1,41; **0,9 → 1,17**;
1,2 → 1,04 (mais atraso nas curvas). Ficou 0,9.
