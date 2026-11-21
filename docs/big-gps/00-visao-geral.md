# Big GPS — visão geral

Navegação offline para todo o Brasil no FOSTON FS-707DC com BigFoston OS: mapa, rotas para
carro, moto, bicicleta e a pé, navegação curva a curva com voz em português, busca offline,
favoritos e recálculo de rota. Usa o GPS que já funciona no Linux (`bfgps`, ver
[docs/gps.md](../gps.md)) e dados do OpenStreetMap preparados no PC.

**Estado (2026-11-21):** fases 1 e 2 feitas (auditoria e provas de viabilidade). O motor de
mapas próprio desenha o DF no aparelho (tela do mapa no shell); o roteamento (quatro perfis,
Contraction Hierarchies) funciona e foi conferido contra Dijkstra; navegação curva a curva,
voz e busca estão por fazer.
Nada aqui está aprovado para uso no trânsito real.

## Princípios

- O aparelho não tem FPU, tem ~100 MB livres e um SD de 2,7 ms por leitura aleatória
  ([02-analise-hardware.md](02-analise-hardware.md)): tudo o que puder é pré-processado no
  PC; no aparelho só aritmética inteira e leitura em blocos.
- Toda decisão vem de medição no aparelho ([01-pesquisa-tecnologias.md](01-pesquisa-tecnologias.md)).
- Dados OSM sob ODbL 1.0, com atribuição visível; nada de mapas proprietários nem
  download em massa de servidores públicos de tiles.

## Documentos

| Arquivo | Conteúdo |
|---|---|
| [01-pesquisa-tecnologias.md](01-pesquisa-tecnologias.md) | candidatos, medições do libosmscout, decisão |
| [02-analise-hardware.md](02-analise-hardware.md) | orçamento de CPU, RAM, SD, ponto flutuante |
| [03-arquitetura.md](03-arquitetura.md) | componentes, fluxo de dados, organização do SD |
| [05-construtor-mapas.md](05-construtor-mapas.md) | `bgmap-build` no PC |
| [06-renderizacao.md](06-renderizacao.md) | rasterizador, estilos, medições de quadro |
| [07-roteamento.md](07-roteamento.md) | grafo, perfis, CH, conferência, medições |

Os demais documentos (roteamento, navegação, voz, busca, interface, dados do Brasil,
desempenho, testes, licenças, instalação, uso, problemas conhecidos, relatório de entrega)
são escritos quando a fase correspondente for feita, com as medições reais.
