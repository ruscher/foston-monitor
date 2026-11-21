# Big GPS — desempenho (medido no aparelho)

FOSTON FS-707DC: ARM1176 ~470 MHz **sem FPU**, ~100 MB livres, SD com 2,7 ms por leitura
aleatória de 4 KiB e 6,9 MB/s sequencial ([02-analise-hardware.md](02-analise-hardware.md)).
Todos os números abaixo foram medidos em 2026-11-21.

## Mapa

| Situação | Quadro |
|---|---|
| DF z15, dirigindo a 60 km/h (média / pior) | 163–174 ms / 284–325 ms |
| DF z17, dirigindo | 55 ms / 152 ms |
| São Paulo z15, dirigindo (Brasil inteiro) | 342 ms / 446 ms (antes de pular áreas < 2 px: 466 / 677 ms) |
| São Paulo z16, dirigindo | 144 ms / 196 ms |
| Navegação com o mapa girado (DF, z16) | 125–180 ms |
| Primeiro quadro com cache frio (Brasil): DF z15 / Rio z15 / Recife z16 / SP z15 / país z5 | 432 / 786 / 424 / 1441 / 1297 ms |
| Abrir o mapa nacional (1,68 GB no FAT32): frio / com a tabela FAT em cache | 1,2–1,8 s / 9–24 ms |

Libosmscout, para comparação: ~9–11 s por quadro sem rasterizar
([01-pesquisa-tecnologias.md](01-pesquisa-tecnologias.md)).

## Rotas (DF, carro, formato 2)

| Situação | Tempo |
|---|---|
| Primeira rota, cache do sistema limpo (encaixe + busca) | 0,3–0,7 s + 0,7–1,1 s |
| Rota repetida (cache quente) | ~9 ms de busca |
| Cálculo completo na interface (encaixe, busca, instruções), frio | 2,4 s |

## Busca

DF: 22–194 ms por consulta (frio). Brasil (PC): 0,2–14 ms.

## Voz

eSpeak NG: frase de 4,97 s em 0,51 s (quente) / 1,92 s (frio); 1,3 MB de RAM.

## Memória e CPU

| Situação | Valor |
|---|---|
| Shell com o mapa aberto | 3–7 MB (RSS) |
| Shell navegando (rota, voz, mapa girando) | 12,5 MB (pico) |
| CPU navegando (1 quadro/s) | ~38 % |
| CPU com o mapa aberto e o aparelho parado (GPS com posição) | quase zero (redesenha só se o centro andar 2 px) |

## O que mais pesou e como foi resolvido

| Problema | Solução | Efeito |
|---|---|---|
| `double` emulado (318 ns/op) | tudo por vértice em inteiro/ponto fixo | — |
| Feições fora da tela | caixa envolvente por feição | 2–3× menos feições |
| Divisão sem hardware | tabela de recíprocos; uma divisão por aresta | −10 % |
| Linhas grossas pela máquina de polígonos | preenchimento convexo próprio | 278 → 163 ms (z15) |
| Prédios de 1–2 px em cidades densas | pular áreas < 2 px | 466 → 342 ms (SP z15) |
| Leituras aleatórias no SD | ordem de Hilbert + topo da CH junto + formato 2 | 454 → 219 leituras por rota |
| Índice nacional na RAM | busca binária no arquivo | 0 MB de índice na RAM |
