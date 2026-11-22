# Big GPS — busca offline

Código: construtor [tools/big-gps/builder/bgsearch-build.cpp](../../tools/big-gps/builder/bgsearch-build.cpp)
(PC, libosmium), formato e normalização [src/ui/biggps/bgsearch_format.h](../../src/ui/biggps/bgsearch_format.h),
consulta [src/ui/biggps/bgsearch.c](../../src/ui/biggps/bgsearch.c), ferramenta
[src/ui/biggps/bgsearch-cli.c](../../src/ui/biggps/bgsearch-cli.c), tela em
[src/ui/screens/scr_map.c](../../src/ui/screens/scr_map.c).

## O que entra no índice

| Tipo | Origem no OSM | Importância |
|---|---|---|
| Lugar | `place=city/town/village/suburb/quarter/neighbourhood/hamlet/locality` com nome | 100 (cidade) … 30 |
| Rua | vias com `highway` e `name`; trechos de mesmo nome no mesmo bairro viram uma entrada (ponto no meio do trecho mais longo) | 75 (rodovia) … 20 (serviço) |
| Local | `amenity`, `shop`, `tourism`, `leisure`, `aeroway=aerodrome`… com nome, em 18 categorias (postos, hospitais, farmácias, mercados, comida, bancos, hotéis…) | 90 (aeroporto), 50 (hospital, shopping), 25 |
| Endereço | `addr:street` + `addr:housenumber` | 10 |

Contexto de cada entrada: bairro mais próximo (até 3 km) ou cidade (até 30 km); endereços
usam `addr:suburb`/`addr:city` quando existem.

## Busca

- Texto normalizado: minúsculas, sem acentos, só letras e números; "de/da/do/e…" ignorados.
- Cada palavra digitada vale como **prefixo** de alguma palavra do nome ou do contexto, em
  qualquer ordem ("hosp base" acha "Hospital de Base do Distrito Federal").
- A palavra com menos ocorrências guia a busca; as outras filtram. Ordem: importância +
  bônus para palavras completas − distância (log) da posição atual (ou do centro do mapa).
- O mesmo lugar mapeado duas vezes (ponto e prédio, a menos de 300 m) aparece uma vez.
- Categorias: os pontos da categoria mais próximos (até 30 km). No índice (formato 2) os
  locais ficam agrupados por categoria e por tile z10 (~40 km), com uma tabela por
  categoria: a busca lê só os tiles em volta da posição (antes percorria o índice inteiro —
  ~76 MB no índice nacional).
- Uma palavra que só existe no contexto ("… curitiba") não serve de guia: a próxima palavra
  mais rara guia uma nova rodada.

## Abreviações e sinônimos (22/11/2026)

A consulta e cada candidato passam pela mesma forma canônica
([bgsearch.c](../../src/ui/biggps/bgsearch.c), tabela `SYN`):

- **Nome por extenso vira a abreviação:** "superquadra norte" → `sqn`, "setor de habitações
  individuais sul" → `shis`, "eixo rodoviário" → `eixao`, "doutor" → `dr`, "avenida" → `av`
  (~70 pares: tipos de via, títulos, Brasília).
- **Rodovias viram uma palavra só:** "BR 020", "br20", "BR-020", "br020" → `br020`
  (BR e as 27 siglas estaduais, 2 ou 3 dígitos).
- **Tipo de via opcional:** "av", "rua", "rodovia"… contam quando o nome tem a palavra (esses
  resultados vêm antes), mas não são exigidos: no OSM é "W3 Norte", não "Avenida W3 Norte".
- **Abreviação vinda de nome por extenso vale só inteira** ("doutor" não acha "Drogaria").
  Palavras de 1 ou 2 letras também valem só inteiras quando há outras palavras
  ("praça da sé" não casa "se" com "setor").
- **Busca parcial:** o candidato é comparado também com o nome por extenso das abreviações
  dele, então "superq" já acha "SQN 312".
- **Nome antes da cidade:** quem tem todas as palavras no nome, e principalmente a
  primeira, vem antes de quem só tem a palavra no bairro ou na cidade ("av paulista sao
  paulo" → Avenida Paulista, São Paulo; antes vinha "Avenida São Paulo", Flórida Paulista).

## Formato 3 (22/11/2026)

O formato 2 lia, no cartão, o nome de cada candidato da palavra mais rara; com o índice
nacional (3,8 milhões de entradas) isso levava de 1 a 18 s com o cache frio. O formato 3:

- **As palavras do bairro e da cidade também entram nas listas:** a consulta cruza as
  listas de todas as palavras (leitura sequencial) e só lê os candidatos que têm todas.
- **Entradas ordenadas por região** (tile z10, ~40 km) e uma tabela de regiões (~60 KB,
  lida ao abrir): pelo número da entrada a busca sabe a região sem ler o cartão, confere
  primeiro as regiões com a melhor nota possível (importância máxima da região e
  distância) e para quando nenhuma região restante pode entrar nos resultados. Um nome
  comum no país inteiro ("rua das flores") para 1500 candidatos depois que a lista enche.
- O shell abre o índice de versão mais alta da pasta `busca/`; um shell antigo abre o
  primeiro em ordem alfabética, então a cópia v2 fica como `brasil-v2.bgs`.
- A busca roda numa thread própria; a tela mostra "Buscando…" e o teclado não para.

Índice nacional: 248 MB (v2: 208 MB), construído em 85 s. No aparelho, cache do sistema
limpo antes de cada consulta, Brasília:

| Consulta | v2 | v3 | 1º resultado (v3) |
|---|---|---|---|
| sqn 312 | 8,6 s | 0,33 s | SQN 312 |
| superquadra norte 312 | 17,6 s, nada | 0,34 s | SQN 312 |
| av w3 norte | nada | 0,28 s | W3 Norte |
| padaria | 2,4 s | 0,16 s | Padaria Vitória (2,3 km) |
| doutor | 17,9 s | 0,94 s | consultório (3,0 km) |
| hospital | — | 0,34 s | Hospital SARAH |
| aeroporto | — | 0,34 s | Aeroporto de Brasília |
| shis qi 5 | 0,9 s | 0,31 s | SHIS QI 5 |
| rua das flores | 13,6 s | 1,3 s | Rua das Flores (Vila Planalto) |
| pelourinho salvador | — | 0,48 s | Pelourinho, Salvador |
| praça da sé | — | 3,1 s | Praça da Sé, São Paulo |
| av paulista sao paulo | — | 5,4 s | Avenida Paulista, São Paulo |
| categoria (postos, hospitais) | — | 52–80 ms | — |

Consultas de outras cidades com palavras muito comuns ("sao", "paulo") ainda levam alguns
segundos com o cartão frio; a segunda consulta parecida usa o cache do sistema.

## Na tela

Botão da lupa ao lado do Voltar → campo de busca com teclado; com o campo vazio, atalhos de
categoria e os **favoritos**. Tocar num resultado centraliza o mapa e abre a folha de
destino com o nome do lugar; **Salvar** guarda nos favoritos
(`$BIGFOSTON_DATA/biggps-favoritos.txt`, uma linha "lat lon nome").

## Medições (DF, 2026-11-21)

| Medida | Valor |
|---|---|
| Construção no PC | 1–2 s; 33 933 entradas (624 lugares), 9 787 palavras, 114 688 referências |
| Arquivo | 1,9 MB |
| Consulta no PC | 0,1–0,3 ms |
| Consulta no aparelho (cache do sistema limpo) | 22–194 ms ("w3 sul" 22 ms, "padaria" 194 ms) |

Exemplos (perto da Rodoviária): "rodoviaria" → Rodoviária do Plano Piloto (0,1 km);
"hospital" → Hospital de Base (1,0 km), HRAN (1,1 km), SARAH (1,1 km); "aeroporto" →
Aeroporto Internacional de Brasília; categoria postos → 0,6 km, 0,7 km…

![Busca (simulador)](../images/big-gps/simulador-busca-inicio.png)
![Resultados (simulador)](../images/big-gps/simulador-busca.png)

## Limitações conhecidas

- "quadra 302 norte" não acha "SQN 302" (a ordem das palavras não bate com nenhuma forma
  conhecida); "superquadra norte 302" e "sqn 302" acham.
- "no", "na", "de"… são ignorados: "superquadra no" (digitando "norte") acha as SQS e SQN.
- Sem interpolação de números de casa (só endereços mapeados um a um).
