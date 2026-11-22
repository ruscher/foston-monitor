# Desempenho do BigFoston OS — visão geral

Projeto de 21/11/2026: medir antes de mudar, atacar a causa e medir de novo, no
aparelho (FOSTON FS-707DC, ARM1176 de 468 MHz, 128 MB, 800×480 RGB565). Branch
`perf/bigfoston-extreme-optimization`.

| Documento | Conteúdo |
|---|---|
| [01](01-hardware-baseline.md) | hardware medido e linha de base |
| [02](02-branch-audit.md) | auditoria das branches |
| [03](03-cpu-profiling.md) | ferramentas (bf-perf-monitor, roteiro do mapa, perfilador) e CPU |
| [04](04-memory-analysis.md) | memória |
| [05](05-zram.md) | zram |
| [06](06-swap-sd.md) | swap no cartão SD (decisão) |
| [07](07-allocators.md) | alocadores (decisão) |
| [08](08-lvgl-optimization.md) | LVGL e framebuffer |
| [09](09-gps-map-engine.md) | motor do mapa e níveis de detalhe |
| [10](10-rendering-fps.md) | quadros por segundo e roteiro de teste |
| [11](11-sd-io.md) | cartão SD |
| [12](12-scheduler-tasks.md) | prioridades, timers e serviços |
| [13](13-kernel-tuning.md) | kernel |
| [14](14-benchmarks-before-after.md) | **antes e depois** |
| [15](15-stress-tests.md) | testes de carga |
| [16](16-regressions.md) | regressões encontradas |
| [17](17-recovery.md) | recuperação e rollback |
| [18](18-architecture-decisions.md) | decisões |
| [19](19-known-limitations.md) | limitações |
| [20](20-final-report.md) | relatório final |

**Resumo:** o arrasto do mapa foi de 3,6 para ~17 quadros/s e a interface deixou de
travar 260 ms por volta do laço; o GPS deixou de ficar ~200 ms/s bloqueado no cartão;
a interface parada acorda 4 vezes/s em vez de 57. Memória não era problema (~85 MB
livres com o mapa nacional e navegação). Detalhes e números em [14](14-benchmarks-before-after.md).
