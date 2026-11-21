# Carga da bateria no Linux (FS-707DC)

Estado: **implementado, desligado por padrão, em teste no aparelho**
(parâmetro `mt3351_ts.charge`, ver abaixo). A leitura de tensão, porcentagem e
estado do carregador já funcionava; a carga em si é feita **por software**, como
no Windows CE.

## Por que a bateria não carregava no Linux

O carregador da PMU do MT3351 tem um *watchdog* em hardware: o `Battdrvr.dll` do
CE o rearma a cada ciclo de 10 s (`0x80061414` ← `0xA250`, espera, `0xA257`) e
liga a carga pelo bit 1 de `0x80061378`. Sem esse software, a carga para. Lido no
aparelho com o Linux rodando (só leitura, 20/11/2026):

| Registrador | Valor | Leitura |
|---|---|---|
| `0x80061378` | `0x0A00` | configuração do CE (tensão constante de 4,2 V), carga desligada |
| `0x80061414` | `0x0000` | watchdog do carregador parado |
| `0x80061420` | `0x0DA9` | carregador presente (bit 3), bateria presente (bit 2 = 0), sem erro |
| `0x80061428` | `0x0048` | sem falha de watchdog (bits 1:0 = 0) nem de RTC (bit 2) |
| `0x8002C018` | `0x0090` | bit 7 = 1: carga permitida |

## O que o Windows CE faz (análise estática do `Battdrvr.dll`)

Módulo na região XIP 1 da NAND (`DSK9-msflash.img`, offset de arquivo `0x20000`,
base `0x8c003000`); `.text` em `0x03bb1000`, sem seções comprimidas. Valores do
registro do CE: `g_ConstantVoltage_vol=4200`, `g_MAX_CV_ChargingTime=7200`,
corrente CC `0xE0` (AC) / `0xF0` (USB), proteção de temperatura **desligada**.

| Endereço | Uso (código do CE) |
|---|---|
| `0x80061378` | bit 1 = carga ligada; `[8:5]` = código de corrente; no Init a palavra inteira vira `0xA00` (4,2 V) |
| `0x80061418` | chave `0xAF26` escrita antes de cada mudança em `0x378` |
| `0x80061414` | `0xA250` → espera → `0xA257` = rearma o watchdog; só `0xA250` ao parar |
| `0x80061410` | bits 6 e 7 ligados a cada ciclo |
| `0x80061404` | bit 0 carregando, bit 4 cheia (LEDs, inferido) |
| `0x80061420` | bit 1 falha, bit 2 sem bateria, bit 3 carregador, bit 4 fase CV, bit 12 estado 0x1001, bit 13 pré-carga, bit 14 carregando, bit 15 medida inválida |
| `0x80061428` | `[1:0]` 2 = timeout do watchdog, 3 = temperatura; bit 2 = falha do RTC 32K |
| `0x8002C018` | bit 7 tem que ser 1 para carregar |
| ADC `0x80034000` | modo auxiliar: `+0x28` VBAT, `+0x2C` VISENSE (×5600/1023), `+0x30` VCHG (×14000/1023) |

Ciclo de 10 s: mede (5 amostras, descartando as com bit 15), confere falhas,
liga a carga por um instante para ler a fase (pré-carga, 0x1001, CC ou CV) e então
carrega. Fim: corrente ≤ 50 em CV (I = 5 × (VISENSE − VBAT)), 2 h em CV ou 8 h no
total; pré-carga além de 30 min trava até tirar o carregador. Recarrega quando
VBAT < 4000 mV. VCHG fora de 4350–6000 mV não carrega.

## Implementação no Linux

`drivers/input/touchscreen/mt3351-ts.c` (o mesmo driver que lê a bateria, dono do
ADC), `delayed_work` de 10 s com o algoritmo acima, e escolhas conservadoras:

- **não reescreve** a palavra `0xA00` da tensão final deixada pelo CE;
- corrente sempre a menor usada pelo CE (`0x80`), ajustável em
  `/sys/module/mt3351_ts/parameters/charge_current` (`0x80`, `0xA0` ou `0xE0`);
- os mesmos limites de tempo e de corrente do CE, e todos os abortos por falha;
- no desligamento, reinício e remoção do driver: carga desligada e watchdog parado
  (o hardware também corta sozinho se o software parar de rearmar);
- **na bateria**: com VBAT + 100 mV < 3450 mV em 3 leituras seguidas (30 s), o
  sistema desliga de forma ordenada (`orderly_poweroff`), como o CE.

DT: `touchscreen@80034000` ganhou as janelas `<0x80061378 0xb4>` (palavras do
carregador; **não** inclui `0x80061374`, a energia do SD) e `<0x8002c018 0x4>`.

### Ligar e acompanhar

```sh
echo 1 > /sys/module/mt3351_ts/parameters/charge     # liga (0 desliga)
dmesg | grep charger                                    # fase, VBAT, I, VCHG
cat /sys/class/power_supply/battery/status              # Charging / Full
```

## Em aberto

- Corrente em mA de cada código e período real do watchdog (medir no aparelho).
- Significado de `0x8002C018` bit 7, `0x80061408` e `0x80061410` bits 6–7.
- O tipo de carregador (USB do PC × carregador dedicado) vem do `usbfn.dll` no CE;
  o Linux trata todos como o caso mais conservador.
