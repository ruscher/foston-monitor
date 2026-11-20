# Touchscreen do FS-707DC no Linux

Estado: **funcionando** no aparelho (kernel 6.12.117-bigfoston, driver `mt3351-ts`),
testado com toques nos 4 cantos, centro, arrastos e toques rápidos (2026-11-20).
Falta: calibração em pixels (espaço de usuário) e interrupção de toque (hoje é polling).

## Origem dos registradores

Tudo veio do `touch.dll` do Windows CE do próprio aparelho (desmontado da imagem XIP)
e foi conferido com `devmem` no Linux antes do driver existir.

Bloco em `0x80034000` (o CE mapeia 100 bytes):

| Offset | Uso | Confirmado |
|---|---|---|
| `+0x3c` | seleção de canal: `0x10` placa horizontal, `0x50` vertical, `0x30` Z1, `0x40` Z2 | sim |
| `+0x40` | escrever 1 inicia conversão; bit 0 ocupado, bit 1 caneta encostada | sim |
| `+0x44` | resultado de 10 bits | sim |
| `+0x38`, `+0x58`, `+0x0c` | configuração (debounce etc.) deixada pelo CE | não alterados |

O CE também mexe em pinmux (`0x80001704`), GPIO (`0x80002000`, `0x80002700`) e
registradores de energia (`0x80001320`/`0x80001340`). **O driver Linux não toca em
nenhum deles**: o bloco já fica configurado pelo CE antes do salto para o Linux.

## Leituras medidas (ADC bruto)

| Ponto | canal 0x50 | canal 0x10 |
|---|---|---|
| sup. esq. | ~775 | ~880 |
| sup. dir. | ~715 | ~140 |
| inf. dir. | ~190 | ~160 |
| inf. esq. | ~190 | ~880 |
| centro | ~475 | ~480 |
| sem toque | 0 | 1023 |

O canal 0x10 é o eixo horizontal e o 0x50 o vertical, ambos invertidos. O DT declara
`touchscreen-inverted-x` e `touchscreen-inverted-y`; o evdev entrega X crescendo para a
direita e Y para baixo, em 0..1023.

## Filtro de pressão

Toque leve lê posições puxadas para o centro do painel. Como no CE
(`TouchResistorThreshold` = 2500), a resistência de contato é
`R = Y * (Z2 - Z1) / Z1`, com Y = canal 0x50:

- toque firme, qualquer posição: R entre 1255 e 2072 (60 amostras);
- dedo pousando ou saindo: R ≥ 4700.

## Algoritmo do driver

A cada `poll_ms` (10 ms), só enquanto algum programa mantém `/dev/input/event0` aberto:

1. Se o bit de caneta estiver em 0, a leitura é ruim.
2. 5 conversões por canal (X, Y, Z1, Z2): descarta o menor e o maior valor e tira a média dos 3 do meio. X e Y são rejeitados se o espalhamento passar de `max_spread`.
3. Rejeita os valores de repouso (X ≥ 1016, Y ≤ 7) e contatos com R acima do limite. O limite é `max_resistance`, ou 1,5× ele com o toque já em andamento, porque a pressão cai durante o arrasto.
4. Relê o bit de caneta. Se o dedo saiu durante a amostragem, a leitura é ruim.
5. A posição é publicada com um ciclo de atraso. Assim, a última leitura antes de soltar o dedo é descartada.
6. O toque só é solto depois de `release_ms` sem nenhuma leitura boa.

Parâmetros ajustáveis em `/sys/module/mt3351_ts/parameters/`:
`max_resistance` (2500), `max_spread` (24), `release_ms` (100), `poll_ms` (10, só no boot).

## Resultado do teste (evdev)

| Toque | Posição relatada |
|---|---|
| sup. esq. | (113, 234) |
| sup. dir. | (877, 288) |
| inf. dir. | (865, 840) |
| inf. esq. | (115, 854) |
| centro | (522, 544) |
| arrasto horizontal | um toque contínuo de 4,7 s, X de 238 a 802 |

Os cantos não chegam a 0/1023 porque o painel resistivo é maior que a área útil. Por isso
é necessária a calibração de 5 pontos em espaço de usuário, que leva aos pixels 800×480.
