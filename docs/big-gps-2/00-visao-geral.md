# Big GPS 2.0: visão geral e andamento

Objetivo: navegação fluida, com cara moderna (referência de experiência: Apple Maps, sem copiar
marca, ícones nem mapas), voz neural Piper Faber e alertas úteis, no FOSTON FS-707DC
(ARM1176 sem FPU, 128 MB de RAM, tela 800×480 resistiva de um toque). Offline sempre.

Branch: `feat/big-gps-apple-maps-experience` (a partir do `main`, 0.5.1).

| Fase | Estado | Documento |
|---|---|---|
| 1. Auditoria e causa dos 2 FPS | feita e medida no aparelho | [01](01-auditoria.md) |
| 2. Movimento separado do GPS (interpolação, câmera, quadros sob demanda) | feita; medida no aparelho com GPS simulado; falta percurso real | [04](04-arquitetura-renderizacao.md), [05](05-gps-interpolacao.md), [30](30-benchmarks.md) |
| 3. Renderização (giro e zoom suaves, margem) | a fazer | |
| 4. Visual novo (painéis, rotas azuis, alternativas) | a fazer | |
| 5. Perspectiva 2.5D | a fazer (estudo de custo primeiro) | |
| 6. Manobras, faixas, zoom em bifurcações | a fazer | |
| 7. Radares, lombadas, semáforos | a fazer | |
| 8. Piper Faber (pré-gerada no PC) | a fazer | |
| 9. Marcadores personalizáveis | a fazer | |
| 10. Pin arrastável, paradas, busca | a fazer | |
| 11–12. Testes completos e entrega | a fazer | |

Regra de todas as fases: medir antes e depois no aparelho e separar "funciona no simulador"
de "funciona no FOSTON". Bluetooth, GPS e telefone nunca são desligados para ganhar
desempenho.
