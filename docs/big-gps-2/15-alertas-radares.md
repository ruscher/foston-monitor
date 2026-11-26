# Alertas: radares, lombadas, passagens de nível e semáforos

## Dados

Pacote à parte, `big-gps/alertas/NOME.bga` ([formato](../../src/ui/biggps/bgalert_format.h)). Os
mapas `.bgm` já instalados continuam valendo, e sem o pacote o Big GPS funciona igual.
Gerado no PC por `tools/big-gps/builder/bgalert-build` (também chamado por `build-data.sh`),
só a partir de **nós do OpenStreetMap** (ODbL):

| Tipo | Etiqueta OSM | Brasil (extrato de 21/11/2026) |
|---|---|---|
| Semáforo | `highway=traffic_signals` (sem os de travessia de pedestre: `highway=crossing`) | 48.555 |
| Radar fixo | `highway=speed_camera` | 9.998 (8.589 com `maxspeed` no próprio nó) |
| Lombada | `traffic_calming=bump/hump/table/cushion/yes/speed_bump` | 93.328 |
| Passagem de nível | `railway=level_crossing` | 17.221 |

O arquivo tem 2,0 MB (12 bytes por ponto). O extrato do Brasil é lido em 8 s. No aparelho o
arquivo é **mapeado** (`mmap`): abrir não custa nada, e só as páginas consultadas são lidas.

O limite de velocidade só é dito quando o **próprio ponto do radar** tem `maxspeed` em km/h.
Nada é deduzido da via. O sentido (`direction`/`camera:direction`, em graus ou rosa dos ventos)
só é usado quando existe. Os dados do OSM são incompletos: **não há garantia de que todo radar
ou lombada esteja no mapa.**

## Na rota (`bga_route`)

Calculado uma vez por rota (17 ms para uma rota de 34 km no aparelho):
- semáforo, lombada e passagem de nível: a no máximo 6 m da linha da rota (são nós da própria via);
- radar: a até 25 m e, se tiver sentido, virado para o lado em que a rota anda (±60°). Assim o
  radar da outra pista não conta;
- o mesmo tipo a menos de 40 m conta uma vez.

## Avisos

- **Voz** uma vez por ponto: radar a partir de max(300 m, 15 s na velocidade atual)
  ("Radar a 400 metros, limite de 60 quilômetros por hora"); lombada e passagem de nível a
  partir de max(120 m, 8 s) ("Lombada à frente"). Não fala nos 2,5 s seguintes a outra fala.
- **Placa do radar** quando o radar tem limite no mapa (pedido do usuário, 26/11/2026). É uma
  placa redonda, branca e com borda vermelha, com o limite em números grandes, "km/h" e a
  distância numa etiqueta embaixo:
  - **aparece grande** (136 px), à esquerda da coluna de botões, junto da borda de baixo;
  - **depois de 4 s diminui** (72 px, animação de 0,4 s) e vai para o canto inferior direito,
    abaixo do botão de voz;
  - a escala e o "© OpenStreetMap" se afastam para o lado e continuam visíveis (ODbL);
  - some quando o radar fica 5 m para trás.

  Radar **sem** limite no mapa continua com o chip amarelo ("Radar · 300 m"): nenhum limite é
  inventado.
- **Bipe acima do limite.** Com a velocidade do GPS acima do limite da placa, um bipe curto
  (1 kHz, 100 ms, rampas de 8 ms):
  - o intervalo encurta conforme o radar se aproxima: 1,5 s a 300 m ou mais, 0,2 s no radar
    (linear);
  - a distância é estimada 20 vezes por segundo pela velocidade, entre as posições do GPS;
  - a etiqueta da distância e a velocidade na coluna da esquerda ficam vermelhas;
  - parou de exceder: os bipes param na hora;
  - o bipe não interrompe uma fala, segue o botão de voz (mudo = sem bipe) e passa pelo mesmo
    caminho da voz, inclusive misturado à música do Bluetooth ([24](24-audio-bluetooth.md)).
- **Chip** amarelo sob a faixa da manobra para lombadas, passagens de nível e radares sem
  limite: "Lombada · 59 m".
- **Semáforos só no mapa**: não geram voz, porque seriam avisos demais. O estado (vermelho,
  verde) não é simulado: o ícone mostra as três luzes.
- Chave "Avisar radar e lombada" no painel do destino (salva em `map_alerts`).

O recurso é de segurança: avisa para respeitar o limite e reduzir a velocidade. Não há
nenhuma função de evasão de fiscalização.

## No mapa

Ícones desenhados pelo rasterizador no quadro do motor, sem imagem nem SVG por quadro:
- radar a partir do zoom 13;
- passagem de nível a partir do 14;
- lombada e semáforo a partir do 15;
- no máximo 160 ícones por quadro, radares primeiro.

## Verificado

- `tests/test_bgalert.c`: distância à via, radar da outra pista, duplicatas, ordem, arquivo
  corrompido.
- No FOSTON (25/11/2026, navegação simulada em Brasília): 169.102 pontos abertos; 31 alertas
  na rota; chip "Lombada · 59 m" e voz "Lombada à frente" com o ícone na via à frente. Ainda
  não visto com radar real nem dirigindo.
- Placa e bipes (26/11/2026), simulador: capturas `42n-biggps-radar-placa` (grande) e
  `42o-biggps-radar-acima` (pequena, acima do limite).
- Placa e bipes no FOSTON (26/11/2026), fase `radar` do bench: chegando a 80 km/h num radar de
  60 km/h a 300 m. A placa encolheu para o canto, abaixo do botão de voz, e a escala foi para
  o lado. Saíram 122 bipes, todos misturados à música que o celular tocava por Bluetooth.
  Ainda não testado dirigindo.
