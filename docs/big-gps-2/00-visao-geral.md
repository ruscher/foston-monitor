# Big GPS 2.0: visão geral e andamento

Objetivo: navegação fluida, com cara moderna (referência de experiência: Apple Maps, sem copiar
marca, ícones nem mapas), voz neural Piper Faber e alertas úteis, no FOSTON FS-707DC
(ARM1176 sem FPU, 128 MB de RAM, tela 800×480 resistiva de um toque). Offline sempre.

Branch: `feat/big-gps-apple-maps-experience` (a partir do `main`, 0.5.1).

| Fase | Estado | Documento |
|---|---|---|
| 1. Auditoria e causa dos 2 FPS | feita e medida no aparelho | [01](01-auditoria.md) |
| 2. Movimento separado do GPS (interpolação, câmera, quadros sob demanda) | feita; medida no aparelho com GPS simulado; falta percurso real | [04](04-arquitetura-renderizacao.md), [05](05-gps-interpolacao.md), [30](30-benchmarks.md) |
| 3. Renderização (giro e zoom suaves, margem) | zoom gradual feito; girar a imagem entre quadros medido e descartado (custo 3,5×); abrir o mapa 433 → 39 ms | [04](04-arquitetura-renderizacao.md) |
| 4. Visual novo (painéis, rotas azuis, alternativas) | parcial: rota percorrida mais clara; o resto espera as imagens de referência | [12](12-rotas-percorridas.md) |
| 5. Perspectiva 2.5D | a fazer (estudo de custo primeiro) | |
| 6. Manobras, faixas, zoom em bifurcações | zoom inteligente feito (testes); guia de faixas a fazer (precisa de dados novos no pacote de rotas) | [13](13-zoom-inteligente.md) |
| 7. Radares, lombadas, semáforos | feita: ícones, avisos na rota, voz e chip; visto no aparelho com simulação | [15](15-alertas-radares.md) |
| 8. Piper Faber (pré-gerada no PC) | feita: frases fixas + 14 mil nomes do DF (596 MB), eSpeak removido; ouvida no aparelho; falta testar com música BT | [22](22-piper-faber.md), [24](24-audio-bluetooth.md) |
| 9. Marcadores personalizáveis | feito: 9 modelos, 8 cores, 3 tamanhos por perfil; testado no aparelho | [18](18-veiculos-personalizados.md) |
| 10. Pin arrastável, paradas, busca | pino arrastável feito e testado; visão geral da rota feita; paradas e rotas alternativas a fazer | [19](19-pin-arrastavel.md), [32](32-visao-geral-rota.md) |
| 11–12. Testes completos e entrega | a fazer | |

Regra de todas as fases: medir antes e depois no aparelho e separar "funciona no simulador"
de "funciona no FOSTON". Bluetooth, GPS e telefone nunca são desligados para ganhar
desempenho.
