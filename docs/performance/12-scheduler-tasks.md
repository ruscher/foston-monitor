# Prioridades, timers e serviços

| Mudança | Por quê | Medido |
|---|---|---|
| thread do motor do mapa com `nice 10` | num núcleo só a interface e o motor dividiam a CPU meio a meio | arrasto 3,6 → 6,0 quadros/s só com isso |
| toque e botão por evento (`poll`) | polling de 33 ms e 40 ms acordava o shell 57 vezes/s | 4 acordadas/s |
| `bigfoston-sd watch` só com builtins | um `grep` por segundo | — (parte do repouso) |
| BusyBox `FEATURE_SH_NOFORK` + `PREFER_APPLETS` | `sleep` dos laços dos serviços sem fork/exec | — |

Nada usa `SCHED_FIFO`/`SCHED_RR`: com um núcleo, uma thread de tempo real com defeito
congelaria o aparelho, e as prioridades normais bastaram. O GPS (`bfgps`) ficou com a
prioridade padrão, acima do motor do mapa. O kernel segue com `PREEMPT_VOLUNTARY`: o teste
de leitura do cartão não mostrou atrasos que justifiquem a preempção total.

Ficou como está (custo pequeno, risco maior que o ganho): a leitura serial do `bfgps`
(~2 % de CPU, imita os tempos de espera de COM do WinCE que o DLL pede) e o polling de
10 ms do driver de toque no kernel (só lê um bit sem dedo na tela).
