# Big GPS 2.0: visão geral e andamento

Objetivo: navegação fluida, com cara moderna (referência de experiência: Apple Maps, sem copiar
marca, ícones nem mapas), voz neural Piper Faber e alertas úteis, no FOSTON FS-707DC
(ARM1176 sem FPU, 128 MB de RAM, tela 800×480 resistiva de um toque). Offline sempre.

Branch: `feat/big-gps-apple-maps-experience` (a partir do `main`, 0.5.1).

| Fase | Estado | Documento |
|---|---|---|
| 1. Auditoria e causa dos 2 FPS | feita, medida no aparelho | [01](01-auditoria.md) |
| 2. Movimento separado do GPS | feita, medida no aparelho (2,5 → 15 q/s na navegação) | [04](04-arquitetura-renderizacao.md), [05](05-gps-interpolacao.md) |
| 3. Renderização | zoom gradual; abrir o mapa 433 → 39 ms; girar entre quadros medido e descartado | [04](04-arquitetura-renderizacao.md), [30](30-benchmarks.md) |
| 4. Visual novo | painel de rotas à esquerda, coluna de navegação, rotas azuis, percorrida mais clara, visão geral | [03](03-design-system.md), [12](12-rotas-percorridas.md), [32](32-visao-geral-rota.md) |
| 5. Perspectiva 2.5D | feita e medida no aparelho (navegação ~12 q/s) | [08](08-perspectiva-2-5d.md) |
| 6. Manobras, faixas, zoom | zoom inteligente e guia de faixas | [13](13-zoom-inteligente.md), [14](14-guia-faixas.md) |
| 7. Radares, lombadas, semáforos | feita, incluindo as lombadas marcadas por você | [15](15-alertas-radares.md), [16](16-alertas-lombadas.md) |
| 8. Piper Faber | voz pré-gerada (DF com nomes); eSpeak removido | [22](22-piper-faber.md), [24](24-audio-bluetooth.md) |
| 9. Marcadores | 9 modelos, 8 cores, 3 tamanhos por perfil | [18](18-veiculos-personalizados.md) |
| 10. Destinos e busca | pino arrastável, paradas, rotas alternativas, busca com correção | [19](19-pin-arrastavel.md), [20](20-paradas-intermediarias.md), [11](11-rotas-alternativas.md), [21](21-busca-offline.md) |
| 11. Testes | automáticos e bench no aparelho; falta dirigir e testar com música BT | [31](31-regressoes.md) |
| 12. Entrega | relatório e limitações | [35](35-relatorio-final.md), [34](34-limitacoes.md) |

Regra de todas as fases: medir antes e depois no aparelho e separar "funciona no simulador"
de "funciona no FOSTON". Bluetooth, GPS e telefone nunca são desligados para ganhar
desempenho.
