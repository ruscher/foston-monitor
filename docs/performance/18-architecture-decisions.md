# Decisões

1. **Medir antes de mexer.** Ferramentas próprias e leves (bf-perf-monitor, roteiro do mapa,
   perfilador por amostragem) em vez de suposições.
2. **Fazer menos trabalho:** não redesenhar o que está coberto, não repetir rótulos, não
   pintar fundo sob imagem, não acordar sem evento, não desenhar edifícios onde o motorista
   não precisa.
3. **Sem swap e sem novo alocador:** a memória sobra e o malloc não aparece no perfil
   ([04](04-memory-analysis.md), [06](06-swap-sd.md), [07](07-allocators.md)).
4. **Sem tempo real:** prioridade normal com `nice` para o motor do mapa.
5. **Sem daemon de memória:** sem pressão para gerenciar; PSI disponível para o futuro.
6. **Compatibilidade:** o patch do LVGL é genérico e vale para todo objeto opaco da camada
   superior; o detalhe Detalhado reproduz o mapa antigo.
