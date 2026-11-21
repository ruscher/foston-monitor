# Big GPS — relatório de entrega

Data: 2026-11-21. Aparelho: FOSTON FS-707DC com BigFoston OS (Linux 6.12).

## O que foi entregue

| Fase | Situação | Evidência |
|---|---|---|
| 1. Auditoria do aparelho | feita | CPU sem FPU (double 318 ns/op), ~100 MB livres, SD 2,7 ms/leitura — [02](02-analise-hardware.md) |
| 2. Pesquisa e provas | feita | libosmscout medido no aparelho: ~10 s/quadro e ~20 s por rota de 25 km → motor próprio — [01](01-pesquisa-tecnologias.md) |
| 3. Mapa do DF | feito | quadro de 163 ms (z15) e 55 ms (z17) dirigindo — [06](06-renderizacao.md) |
| 4. Rotas | feitas | 4 perfis, Contraction Hierarchies; 0 diferenças contra Dijkstra em 1 200 consultas; ~1 s a primeira rota no aparelho — [07](07-roteamento.md) |
| 5. Navegação | feita (simulada) | manobras, acompanhamento, recálculo, chegada; demo no aparelho — [08](08-navegacao.md) |
| 6. Voz pt-BR | feita | eSpeak NG no sistema; avisos no momento certo; ~10× mais rápido que o tempo real — [09](09-voz.md) |
| 7. Busca | feita | ruas, lugares, locais, endereços, categorias, favoritos, recentes — [10](10-busca.md) |
| 8. Brasil inteiro | mapa e busca feitos; rotas de carro em construção na data | mapa nacional de 1,68 GB funcionando no aparelho com GPS real; busca de 3,8 milhões de entradas — [13](13-dados-brasil.md) |
| 9. UX | feita | tela cheia, nomes de vias ao longo da via, dia/noite, favoritos, recentes — [11](11-interface.md) |
| 10. Desempenho | medido | [14](14-desempenho.md) |
| 11. Testes de estresse | feitos (sem teste dirigindo) | 120 arquivos corrompidos e a interface inteira sob ASan/UBSan: 0 falhas; navegação longa no aparelho — [15](15-testes.md) |
| 12. Entrega | este documento | licenças [16](16-licencas.md), instalação [17](17-instalacao-sd.md), uso [18](18-uso.md), problemas [19](19-problemas-conhecidos.md) |

## Números principais (medidos no aparelho)

- Mapa: 55–175 ms por quadro no DF; 144 ms em São Paulo z16; 342 ms em São Paulo z15.
- Rota: ~1 s com o cartão frio, ~9 ms com cache; 2,4 s do toque ao resultado na interface.
- Memória do shell navegando: 12,5–13,4 MB; CPU ~38 % navegando, ~0 parado.
- Busca: 22–194 ms (DF, frio); índice nacional de 204 MB.
- Voz: frase de 5 s sintetizada em 0,5 s.

## O que não foi feito ou não foi verificado

- **Nenhum teste dirigindo.** As rotas não estão aprovadas para o trânsito real.
- Confirmação auditiva da voz e teste com música tocando.
- Rotas nacionais de bicicleta e a pé (o modo por nós já reduz o tamanho; falta construir).
- Semáforos, pedágio, trânsito; sinônimos na busca; interpolação de números de casa.

## Próximos passos recomendados

1. Instalar no cartão (leitor do PC) o conjunto nacional e testar dirigindo, com o GPS ao
   ar livre; anotar trechos com instruções ruins.
2. Construir as rotas nacionais de bicicleta e a pé.
3. Depois do Big GPS: Bluetooth (UART2, MediaTek) — engenharia reversa necessária.
