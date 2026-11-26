# Paradas intermediárias

- No painel da rota, **"+ Parada"**: aparece o aviso laranja "Toque e segure onde fica a parada",
  e o próximo toque longo no mapa marca a parada e recalcula.
- Até **3 paradas**, na ordem em que foram marcadas, com círculos laranja numerados no mapa
  (projetados também na vista 3D).
- **"Tirar paradas"** volta à rota direta.
- Mover o pino do destino mantém as paradas. Escolher outro destino (busca ou toque longo) as
  apaga.
- Na navegação:
  - cada parada é uma manobra "Você chegou à parada", com o ícone de chegada e a voz Faber;
  - ao recalcular por sair da rota, as paradas já alcançadas saem da lista.

## Como (sem mexer no Contraction Hierarchies)

O roteador (`bgrouter.c`) faz **uma consulta comum por trecho**: início → parada 1 → … →
destino. Cada trecho vira um `bgn_route_t`, e `bgn_append` (`bgnav.c`) os emenda:
- pontos, distâncias e tempos continuam do trecho anterior;
- a chegada do trecho anterior vira `BGN_WAYPOINT`;
- a saída do trecho seguinte some;
- se os dois trechos não se encostam (o encaixe na via pode cair alguns metros adiante), essa
  distância entra no total.

## Ainda não

- Rotas alternativas: hoje feitas por penalidades ([11](11-rotas-alternativas.md)); com paradas, a
  rota vai sem alternativas.

## Verificado

- `tests/test_bgnavlegs.c`:
  - emenda com o ponto repetido;
  - emenda com folga de 10 m;
  - manobras e distâncias;
  - texto e voz da parada.
- No FOSTON, com toques injetados e GPS falso na hora atual:
  - rota de 2,6 km / 4 min;
  - "+ Parada" e toque longo: parada "1" no mapa e rota de 2,7 km / 5 min passando por ela,
    calculada em 417 ms;
  - o botão "Tirar paradas" aparece.
- Voz: "Você chegou à parada" no pacote de frases fixas (129 frases). A voz agora procura em
  todos os pacotes `.bgv`, o maior primeiro: dá para acrescentar frases sem refazer o pacote
  regional.

## Reordenar, tirar uma e arrastar (BigFoston OS 0.6.3, 26/11/2026)

- **No painel de rotas**, abaixo de Iniciar/Simular/+ Parada, cada parada tem uma linha:
  - o número (o mesmo do círculo laranja no mapa);
  - ▲ e ▼ para trocar a ordem (apagados quando não há para onde ir);
  - ✕ para tirar só aquela.
- Cada mudança recalcula a rota. O painel rola quando as paradas passam da altura.
- O botão "Tirar paradas" (todas) saiu: o ✕ de cada linha faz isso, e o botão cortava o texto
  "Simular".
- **No mapa**, segurando o círculo de uma parada, ela passa a acompanhar o dedo:
  - fica ampliada, 30 px acima do dedo, à vista;
  - o mapa anda quando o dedo chega à borda;
  - ao soltar, a rota é recalculada passando por ali.
- O mesmo vale para o pino do destino, que já era arrastável.
- Os botões agem fora do evento do próprio botão (`lv_async_call`): a rota nova recria o painel, e
  apagar o pai dentro do evento do filho derrubaria o LVGL.

Verificado no FOSTON, com a fase `paradas` do bench e toques injetados com o `bftap`:
- rota com 2 paradas (414 trechos);
- ▼ na parada 1 trocou a ordem (780 trechos: a rota agora vai e volta);
- a parada 1 foi arrastada 70 px com toque longo, e a rota passou de 41 min / 41 km para
  34 min / 33 km.

Captura do simulador: `42q-biggps-paradas`.
