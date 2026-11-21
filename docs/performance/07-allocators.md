# Alocadores — decisão

Os perfis do shell no arrasto e no repouso não mostram `malloc`/`free` entre as funções
com mais amostras (abaixo de 1 %: `lv_free` 0,3 %, `alloc_slot` 1 % numa amostra de
~300). O motor do mapa não aloca por quadro (buffers de borda e de tiles reaproveitados).
As alocações por quadro que existiam na interface (bitmaps dos rótulos) passaram a ter
cache entre quadros ([08](08-lvgl-optimization.md)).

**Decisão: manter o malloc da musl** (o LVGL usa `LV_STDLIB_CLIB`). Trocar por jemalloc
ou mimalloc acrescentaria centenas de KB a um binário estático e não teria onde ganhar:
um núcleo só (as arenas por thread do jemalloc não ajudam), processos pequenos e pouca
alocação no caminho quente. Sem benchmark de jemalloc no aparelho: com o malloc fora do
perfil, o ganho máximo possível é menor que 1 %, e a medida não mudaria a decisão.
