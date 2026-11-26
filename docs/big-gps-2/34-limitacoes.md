# Limitações conhecidas

- **Piper no aparelho:** inviável (medido: 2,8 bilhões de multiplicações por segundo de fala
  contra 3 milhões por segundo no FOSTON). A voz é a mesma Faber, gerada no PC. Nomes de vias
  só para as regiões geradas (o DF hoje).
- **Música do Bluetooth:** some durante a fala, em vez de abaixar. Abaixar exige mixagem
  (`dmix`) na cadeia de áudio do sistema ([24](24-audio-bluetooth.md)).
- **Giro do mapa:** em degraus a cada quadro do motor. Girar entre quadros foi medido e
  descartado.
- **3D:** os nomes no alto ficam mais estreitos (projetados); não há horizonte; quadros do
  motor ~2,4× mais caros.
- **Rotas alternativas:** heurística de pontos de passagem sobre o Contraction Hierarchies.
  Rotas reais, mas não necessariamente as que um algoritmo de alternativas completo daria.
- **Faixas:** só onde o OSM tem `turn:lanes` (162 cruzamentos no DF).
- **Limite de velocidade da via:** não há dado no pacote de rotas. O limite só aparece no
  radar, quando o próprio ponto informa.
- **Paradas:** até 3, sem reordenar nem arrastar.
- **Visual:** as 4 imagens de referência nunca chegaram. O desenho segue as descrições.
- **Ainda não testado no FOSTON** (o aparelho ficou desligado depois do `reboot`):
  - rotas alternativas;
  - faixas;
  - painéis novos;
  - busca corrigida;
  - avisos seus;
  - correção do Bluetooth no modo Performance.
- **Ainda não testado dirigindo.**
