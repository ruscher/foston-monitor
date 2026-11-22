# Big GPS — auditoria de licenças

Situação em 2026-11-21. "No aparelho" = vai no sistema ou no cartão; "só no PC" = usado
para preparar os dados, não é distribuído com o aparelho.

## Código

| Componente | Licença | Onde | Observação |
|---|---|---|---|
| Big GPS (motor `src/ui/biggps`, tela, construtores `tools/big-gps`) | GPL-3.0-or-later | aparelho e PC | código deste projeto |
| bigfoston-shell (LVGL 9.2.2) | MIT (LVGL) | aparelho | já usado pelo shell |
| eSpeak NG 1.52.0 (programa) | GPL-3.0-or-later | aparelho (`/usr/bin/espeak-ng`) | compatível com a GPL-3.0 do projeto; patch próprio para compilar só em C (`firmware/buildroot/package/bigfoston-espeak-ng`) |
| eSpeak NG — partes internas | Apache-2.0 / BSD-2-Clause / Unicode (ucd-tools) | aparelho (no mesmo binário) | arquivos `COPYING.APACHE`, `COPYING.BSD2`, `COPYING.UCD` do eSpeak NG; todas compatíveis com GPL-3.0 |
| libosmium 2.20.0 | BSL-1.0 | só no PC | leitura do PBF nos construtores |
| protozero 1.7.1 | BSD-2-Clause | só no PC | |
| RoutingKit (commit 54d49bb) | BSD-2-Clause | só no PC | grafo e Contraction Hierarchies; a consulta no aparelho é código próprio |
| pyosmium 4.3.2 | BSD-2-Clause | só no PC | recorte de regiões |
| pyshp 3.1.9 | MIT | só no PC | leitura dos polígonos de oceano |
| libosmscout | LGPL-3.0 | não usado | só medido na fase 2 ([01](01-pesquisa-tecnologias.md)) |

## Dados

| Dado | Licença | Obrigações |
|---|---|---|
| OpenStreetMap (extratos da Geofabrik) | **ODbL 1.0** | Os arquivos `.bgm`, `.bgr` e `.bgs` são **bancos de dados derivados**: quem os distribuir deve oferecê-los sob ODbL (ou oferecer os meios de reconstruí-los — os construtores e o processo estão neste repositório, GPL-3.0). Atribuição "© colaboradores do OpenStreetMap" visível no mapa (canto inferior direito, sempre) e gravada dentro de cada `.bgm`. |
| Polígonos de oceano (osmdata.openstreetmap.de, a partir da costa do OSM) | ODbL 1.0 | mesma atribuição do OSM |
| Dados de voz pt do eSpeak NG (`espeak-ng-data`) | GPL-3.0-or-later | vão no cartão; fonte em github.com/espeak-ng/espeak-ng, versão 1.52.0 |

Downloads feitos uma vez por construção, dos servidores de distribuição próprios para isso
(download.geofabrik.de, osmdata.openstreetmap.de). Nenhum servidor de tiles do OpenStreetMap
é usado. Nenhum mapa ou dado do Google Maps, Waze, HERE, TomTom ou iGO.

## Fontes, ícones, imagens

| Item | Licença |
|---|---|
| Fontes do shell (Montserrat) | SIL OFL 1.1 |
| Símbolos (Font Awesome, nas fontes do shell) | SIL OFL 1.1 (fontes) / CC BY 4.0 (ícones) |
| Ícones de manobra, marcadores, lupa | gerados por código deste projeto (GPL-3.0) |
| Capturas de tela em `docs/images/big-gps` | contêm dados do OSM: "© colaboradores do OpenStreetMap" (ODbL) |

## Componentes proprietários

O motor do GPS (`gps.dll` da MediaTek) continua extraído da memória do próprio aparelho e
fica só no cartão dele ([docs/gps.md](../gps.md)); nada do Big GPS depende de outro
componente proprietário.
