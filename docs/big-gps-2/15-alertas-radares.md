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
- **Chip** amarelo sob a faixa da manobra enquanto se aproxima: "Radar · 300 m · 60 km/h".
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
