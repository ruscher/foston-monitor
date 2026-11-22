# Big GPS — busca, semáforos, pedágios e moto

Detalhes: busca em [big-gps/10-busca.md](../big-gps/10-busca.md), rotas em
[big-gps/07-roteamento.md](../big-gps/07-roteamento.md).

## Busca (FUNCIONANDO NO HARDWARE)

- Abreviações e nomes por extenso equivalentes (SQN ↔ superquadra norte, SHIS, Av., Dr.,
  ~70 pares), rodovias numa palavra só (BR 020 = BR-020 = br020), tipo de via opcional ("av w3
  norte" acha "W3 Norte"), nome antes da cidade na ordem.
- Índice formato 3: palavras do bairro e da cidade nas listas e entradas por região com
  tabela em memória. Índice nacional de 248 MB instalado no cartão (o v2 ficou como
  `brasil-v2.bgs`, que o shell antigo continua usando).
- Busca numa thread própria ("Buscando…"), o teclado não para.

| Índice nacional, cache frio, no aparelho | v2 | v3 |
|---|---|---|
| sqn 312 | 8,6 s | 0,33 s |
| padaria | 2,4 s | 0,16 s |
| doutor | 17,9 s | 0,94 s |
| praça da sé | 19 s (resultado errado) | 3,1 s (Praça da Sé, São Paulo) |
| av paulista sao paulo | — | 5,4 s (Avenida Paulista, São Paulo) |

## Semáforos (construtor pronto; dados nacionais a reconstruir)

`highway=traffic_signals` e `crossing=traffic_signals`: semáforos a menos de 30 m formam um
cruzamento, e a espera conta uma vez no trecho que entra nele (carro 10 s, moto 8 s,
bicicleta 6 s, a pé 0; `BGR_SIGNAL_DS`). DF: 880 semáforos → 677 trechos; Rodoviária →
Taguatinga 21 min 21 s → 22 min 02 s, mesmo caminho. Conferência CH × Dijkstra: 0 diferentes.

## Pedágios (construtor e aparelho prontos; dados nacionais a reconstruir)

- `toll=yes` e cabines (`barrier=toll_booth`, que marcam a via): Centro-Oeste 163 cabines.
- Arquivo `-carro-sem-pedagio.bgr`: pedágio custa +1 h só na hierarquia; o aparelho desconta e
  mostra o tempo real. A tela tem **Evitar pedágios** (ajuste salvo) e avisa "Rota com
  pedágio", "Sem caminho sem pedágio" ou "Sem os dados de rotas sem pedágio".

| Centro-Oeste (PC) | Normal | Sem pedágio |
|---|---|---|
| Brasília → Goiânia | 207 km, 2 h 27, 98 trechos com pedágio | 254 km, 3 h 14, nenhum |
| Brasília → Anápolis | 152 km, 1 h 48, 42 trechos | 186 km, 2 h 40, nenhum |
| Cuiabá → Rondonópolis | 218 km, 2 h 51, 49 trechos | 307 km, 5 h 16, nenhum |
| Goiânia → Rio Verde | 231 km, 2 h 22, nenhum | igual |

Conferência do desconto da penalidade: com uma penalidade de só 5 s (a rota passa pelo
pedágio), o arquivo "sem pedágio" deu exatamente o mesmo tempo da rota normal nas três rotas
pedagiadas (2 h 27 min 28 s, 19 min 48 s, 2 h 51 min 09 s): o tempo mostrado é o real mesmo
quando não há alternativa.

Limite: o que o OSM não marca como pedágio não é evitado; a tela diz "pode ter pedágio"
quando faltam os dados e nunca garante uma rota sem pedágio além do que os dados dizem.

## Moto (construtor pronto; dados nacionais a reconstruir)

Arquivo próprio (`-moto.bgr`; sem ele, o de carro como antes): acesso `motorcycle=*` /
`motor_vehicle=*`, máximo 110 km/h, 70 % da velocidade (até 50 km/h) em terra, cascalho e
`tracktype` 2–5, trilhas até 25 km/h, conversões mais baratas (esquerda 6 s, direita 2 s).
Rodoviária → Taguatinga: 21 min 38 s (carro 22 min 02 s).

**Não feito:** rota mais curta, preferência por rodovias e evitar terra como opções
separadas: com a hierarquia de pesos fixos cada uma seria mais um arquivo nacional de ~4 GB;
a moto já evita terra pela velocidade.

## Zoom 15 em cidades densas — não feito nesta etapa

Medido antes ([performance/09](../performance/09-gps-map-engine.md)): z15 econômico 95 ms no
motor (Brasília). Fica para a próxima etapa, com São Paulo, Rio e Belo Horizonte no roteiro.

## Teste com GPS real em movimento — não feito

Rotas continuam **não aprovadas para uso no trânsito**. Falta o percurso com motorista e
outra pessoa anotando, gravação do NMEA e das manobras.
