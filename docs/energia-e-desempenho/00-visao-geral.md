# Energia e desempenho do sistema inteiro

Estado em 24/11/2026, medido no FOSTON FS-707DC (ARM1176 a 468 MHz fixos, 128 MB).

Quatro peças agora trabalham juntas:
- **Plano de energia** (Ajustes > Potência);
- **Efeitos visuais** (Ajustes > Desempenho);
- **Modo performance** (Ajustes > Modo performance);
- prioridade dos processos.

O pedido era que o desempenho atendesse a todos os apps e ao sistema, não só ao mapa. Também
pedia que a TV ligasse o modo performance e que a internet pelo celular e o pareamento do POCO
X4 funcionassem.

| Etapa | Documento | O que entrega | Estado |
|---|---|---|---|
| 1 | [Plano de energia](01-plano-de-energia.md) | Economia, Equilibrado, Desempenho: um toque ajusta tudo | **FUNCIONANDO NO APARELHO** |
| 2 | [Modo performance](02-modo-performance.md) | o que pausa, quando liga, TV, papel de parede, Bluetooth em uso | **FUNCIONANDO NO APARELHO** |
| 3 | [TV sem travar](03-tv-fluidez.md) | três falhas corrigidas no player e a escada de decodificação | **FUNCIONANDO NO APARELHO** |
| 4 | [Internet pelo celular](04-internet-pelo-celular.md) | Bluetooth PAN (tethering) com DHCP e DNS | **FUNCIONANDO NO APARELHO** |
| 5 | [Pareamento do celular](05-pareamento-celular.md) | por que o POCO pedia o código toda vez e o que mudou | PARCIAL: correções no aparelho, captura HCI em andamento |
| 6 | [Medidas e testes](06-medidas-e-testes.md) | CPU por processo, roteiros, comandos | — |

## Em números

| Medida | Antes | Depois |
|---|---|---|
| CPU parado na Início, plano Equilibrado | 15,7 % | 15,7 % (o padrão não muda) |
| CPU parado na Início, plano Desempenho | — | **8,2 %** (GPS e tarefas pausados) |
| TS perdido na TV, sinal bom (SBT, SNR 17–19) | 52 falhas em 17 s (uma por segundo) | **0** |
| TV no SBT (H.264 Main, 30 qps com quadros B) | CPU 100 %, quadros atrasados descartados, TS transbordando | 4–5 qps limpos, 0 drops, CPU 58–84 % |
| TV na Globo (Baseline, 30 qps) | 7–8 qps (o decodificador travava na fila) | **12–26 qps**, 0 drops |
| Internet pelo POCO X4 | não havia | `bnep0` 10.124.78.88, ping, HTTP e HTTPS no app Internet |
| `bfphone` com o aparelho parado | 91 % da CPU (laço girando) | 0,4 % |

## Onde está cada coisa

| Arquivo | Papel |
|---|---|
| `src/ui/core/powerplan.c/.h` | `powerplan_apply` (plano) e `fx_level_apply` (efeitos) |
| `src/ui/core/perfui.c` | política do modo performance: motivos, serviços, papel de parede, etiqueta |
| `src/ui/core/perfmode.c` | liga e desliga os serviços (`/etc/init.d`) e a prioridade |
| `src/ui/core/app.c` | `app_wallpaper_plain` (fundo liso) separado do preto da TV |
| `src/ui/screens/scr_power.c` | Potência: bateria e os três planos |
| `src/ui/screens/scr_perf.c` | Desempenho: plano atual, efeitos visuais e detalhe do mapa |
| `src/ui/screens/scr_perfmode.c` | Modo performance: "Ligar sozinho quando abrir a TV", "Tirar o papel de parede" |
| `src/tv/bftvd.c`, `src/tv/siano.c` | player da TV (no cartão: `BigFoston/tv/bftvd`) |
| `src/bt/bfbt-agent.c` | PAN, DHCP, comando `internet`, gravação das chaves pareadas pelo celular |
| `src/ui/screens/scr_bluetooth.c` | globo de internet na linha do celular |
