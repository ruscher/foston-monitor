# Big GPS — problemas conhecidos e limitações

## Ainda não verificado

- **Teste dirigindo** — toda a navegação foi testada com percurso simulado; falta GPS real
  em movimento (encaixe na via, recálculo com desvio de verdade, zoom pela velocidade).
- **Voz** — o espeak-ng roda e os avisos saem no momento certo (registro do shell), mas falta
  a confirmação de quem ouviu no alto-falante, e o teste com o Multimídia tocando junto.
- **Rotas nacionais de bicicleta e a pé** — a construção do Brasil inteiro para esses perfis
  ainda não foi feita/medida (o de carro estava em construção na data deste documento).

## Limitações

| Área | Limitação |
|---|---|
| Rotas | Sem semáforos, pedágio ou trânsito; velocidades por tipo de via e `maxspeed` (estimativas). Moto usa as regras de carro. Custo de conversão fixo. |
| Rotas | Retorno no meio da via só em rua sem saída (carro/moto/bicicleta). |
| Instruções | Trevos complexos ainda geram avisos encadeados ("mantenha-se à direita" seguidos); artigos das vias por heurística ("na/no"). |
| Busca | Nomes como no OSM: sem sinônimos ("quadra 302 norte" não acha "SQN 302"); sem interpolação de números de casa. |
| Mapa | Cidades muito densas em z15 (São Paulo) ficam em ~340 ms por quadro; sem relevo; rótulos de vias só nos trechos retos mais longos. |
| Dados | Ficam como estão no OpenStreetMap na data do extrato; atualizar = gerar e copiar de novo. |
| Cópia | Pelo cabo USB só ~1 MB/s (sem DMA na USB do aparelho): use o leitor de cartão para o Brasil. |
| FAT32 | Arquivos de até 4 GB: o mapa se divide sozinho em partes; rotas de um perfil precisam caber em 4 GB. |
| Primeira abertura | Abrir o mapa nacional pela primeira vez após ligar leva ~1,5 s (o FAT percorre a cadeia de clusters do arquivo). |
