# Swap no cartão SD — decisão

**Não usar.** Motivos, com as medidas de [04](04-memory-analysis.md) e [11](11-sd-io.md):

1. Não há pressão de memória: ~85 MB livres no pior roteiro, sem faltas maiores.
2. O cartão é FAT32 com mapas, rotas e dados do usuário; o kernel aceita swapfile em
   vfat (via `bmap`), mas não há garantia de que outra escrita no FAT não realoque os
   blocos depois da ativação, e um erro corromperia dados do usuário.
3. Reparticionar o cartão exigiria backup e procedimento de recuperação, e o ganho seria
   nulo (nada para paginar).
4. Swap e mapa disputariam o mesmo cartão de 7 MB/s.

Se um dia houver pressão real, a ordem recomendada é zram pequeno ([05](05-zram.md)) e,
só depois, partição Linux dedicada num cartão próprio, com backup.
