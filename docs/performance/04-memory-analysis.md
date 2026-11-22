# Memória

Medido com `bf-perf-monitor` durante o roteiro completo do mapa (mapa nacional de
1,68 GB, rotas DF, navegação simulada):

| Situação | MemAvailable | RSS do shell |
|---|---|---|
| tela inicial | 103 MB | ~4 MB |
| mapa aberto | ~87 MB | 6-8 MB |
| navegação com rota | ~85 MB (mínimo do roteiro: 84,8 MB) | 12,4 MB |

Sem swap, **zero faltas de página maiores** e zero `pswpout` em todos os roteiros. O
consumo do mapa é limitado por projeto: buffers do mapa 2 × 768 KB, cache de tiles do
motor (~1-4 MB conforme o zoom, medido pelo `bgview`), páginas da rota em cache de 8 MB
no máximo, buffer do LVGL 2 × 768 KB (tela inteira, ver [08](08-lvgl-optimization.md)).

**Conclusão:** não há pressão de memória no uso real; os travamentos eram de CPU
(desenho) e de espera no cartão, não de memória. Por isso nenhuma swap é ativada no boot
([05](05-zram.md), [06](06-swap-sd.md)) e o alocador não foi trocado ([07](07-allocators.md)).
Não foi criado um daemon de memória: sem pressão para gerenciar, ele só gastaria CPU; o
PSI ficou disponível no kernel (`/proc/pressure`) para medir se isso mudar.
