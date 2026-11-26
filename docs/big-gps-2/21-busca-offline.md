# Busca offline

A busca continua sendo o índice binário em C (`bgsearch.c`, 3,8 milhões de entradas do
Brasil): nada passou para Python, AWK ou JSON. Ela roda numa thread, com o último pedido
vencendo, sem travar o teclado.

## O que já havia (e foi conferido)

Normalização de acentos e maiúsculas; prefixo da palavra sendo digitada; abreviações nos dois
sentidos (Av./Avenida, R./Rua, Dr., Sta., Jd., Qd. e outras); siglas de Brasília (SQN, SQS,
SHIS, SHIN, CLN, QI etc.); códigos de rodovia (BR-020, br 020, br20, DF-001). Palavras do
contexto (bairro, cidade) também casam. A ordem é por distância e importância.

## Novo: erros de digitação

Quando o texto não acha **nada**, `bgs_correct` tenta corrigi-lo e a busca roda de novo.
A lista mostra **"Resultados para "…""** com o texto usado.

- Só palavras de 4 letras ou mais, que não sejam número nem abreviação.
- Corrige a palavra se ela não existe inteira no índice, ou se é rara (menos de 20 ocorrências)
  e há uma vizinha **50× mais comum**. Exemplo: "avenda" é um sobrenome em algum lugar, mas
  "avenida" está em toda parte. A palavra ainda sendo digitada vale como prefixo.
- Vizinha = 1 edição (2 para palavras de 8 letras ou mais): letra trocada, faltando, sobrando
  ou duas vizinhas invertidas (Damerau-Levenshtein).
- Fica a de **menos edições** e, no empate, a mais comum.
- As candidatas não são o índice todo: são as palavras com as mesmas duas primeiras letras e,
  para um erro na 2ª letra, os prefixos de 3 letras que a palavra teria (falta, troca, sobra,
  inversão). Os registros são lidos em blocos de 512.

Medido no PC (`bgsearch` do índice do Brasil, posição na Rodoviária de Brasília):

| Digitado | Usado | Primeiro resultado | Tempo |
|---|---|---|---|
| hospitl brasilia | hospital brasilia | Hospital Universitário de Brasília, 2,7 km | 37 ms |
| eixo monumetal | eixo monumental | Eixo Monumental, Bloco O | 11 ms |
| farmacai | farmacia | Farmácia do Trabalhador, 0,7 km | 4 ms |
| parque da cidde | parque da cidade | Parque da Cidade Dona Sarah Kubitschek | 105 ms |
| tagautinga | taguatinga | Taguatinga | 4 ms |
| w3 nrte | w3 norte | W3 Norte | 96 ms |
| avenda paulista sao paulo | avenida paulista sao paulo | Avenida Paulista, São Paulo | 922 ms |
| ceilandai / sobradiho | ceilandia / sobradinho | Ceilândia / Sobradinho II | 3–4 ms |

Três bugs da correção foram encontrados nesses testes e corrigidos antes de entrar:
- a palavra rara "avenda" existir bloqueava a correção;
- uma vizinha rara em "nrt…" escondia "norte";
- "tagautinga" virava "tabatinga" (mais comum, mas a 2 edições em vez de 1).

## Limites

- "farmacia asa norte" continua sem resultado: as farmácias estão no índice, mas o contexto
  delas é "SQN 312, Brasília", e não "Asa Norte". Para isso servem as categorias perto de
  você (Farmácia, Hospital, Posto…).
- No FOSTON, a correção lê mais do cartão que uma busca comum. Ainda não foi medida lá; roda
  na thread da busca.
- CEP: o índice não tem CEP (o OSM do Brasil quase não tem `addr:postcode`).
