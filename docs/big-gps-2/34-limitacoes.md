# Limitações conhecidas

- **Piper no aparelho:** inviável (medido: 2,8 bilhões de multiplicações por segundo de fala
  contra 3 milhões por segundo no FOSTON). A voz é a mesma Faber, gerada no PC. Nomes de vias
  só para as regiões geradas: o DF e o resto do Centro-Oeste (GO, MT e MS)
  ([22](22-piper-faber.md)). Fora delas, a voz diz a manobra sem o nome da via.
- **Giro do mapa:** em degraus a cada quadro do motor. Girar entre quadros foi medido e
  descartado.
- **3D:** os nomes no alto ficam mais estreitos (projetados); não há horizonte; quadros do
  motor ~2,4× mais caros.
- **Rotas alternativas:** heurística de pontos de passagem sobre o Contraction Hierarchies.
  Rotas reais, mas não necessariamente as que um algoritmo de alternativas completo daria.
- **Faixas:** só onde o OSM tem `turn:lanes` (162 cruzamentos no DF).
- **Limite de velocidade da via:** só onde o OSM tem `maxspeed` numérico. Alças e ruas
  pequenas muitas vezes ficam sem placa ([36](36-limites-de-velocidade.md)).
- **Paradas:** até 3.
- **Visual:** as imagens de referência nunca chegaram (não estão em nenhuma conversa do
  projeto). O desenho segue as descrições, com paleta própria ([03](03-design-system.md)).
- **Desempenho com o GPS procurando satélites:** o `bfgps` usa 30–40 % da CPU nessa hora, e
  o mapa fica mais lento que o medido ([30](30-benchmarks.md)). Falta medir com o GPS
  fixado, no carro.
- **Ainda não testado dirigindo.**
