# Guia de faixas

## Dados

Arquivo à parte, `big-gps/rotas/NOME.bgl` ([formato](../../src/ui/biggps/bglanes_format.h)), gerado
por `tools/big-gps/builder/bglanes-build` (também no `build-data.sh`) a partir das etiquetas
**`turn:lanes`, `turn:lanes:forward` e `turn:lanes:backward`** do OpenStreetMap. Os pacotes de
rotas `.bgr` não mudam: o cabeçalho deles tem um número fixo de seções.

- Cada entrada é um **cruzamento e o rumo de chegada**: a via vale no último nó (a favor dela)
  ou no primeiro (contra ela).
- `turn:lanes` sem sentido só vale em via de mão única, rotatória ou autoestrada. Numa via de
  mão dupla seria ambíguo, e é ignorado.
- Valor desconhecido na etiqueta: a via inteira é ignorada. Nada é adivinhado.
- O construtor faz duas passadas, sem índice de coordenadas do país inteiro: Brasil em 13 s,
  **14.981 cruzamentos**, 410 KB. No DF são só 162: a cobertura do OSM é pequena e o guia
  aparece só onde há dados.

## Na navegação

Para a próxima manobra, procura o cruzamento a até 25 m do ponto da manobra, com o rumo de
chegada a até 35° (calculado 30 m antes da manobra). Isso é feito uma vez por manobra
(`bgl_find`, arquivo mapeado, busca binária).

`bgl_fit` decide quais faixas servem:
- virar à esquerda ou à direita (leve, acentuado): as faixas com essa marcação;
- seguir em frente: as de frente; faixa sem marcação conta como frente;
- mantenha-se ou acesso à esquerda/direita: as marcadas para aquele lado. Numa bifurcação em
  que só a saída é marcada ("frente | frente | levemente à direita"), as de frente do outro
  lado;
- rotatória, chegada, ou nada que combine: **o guia não aparece**, e fica só a seta geral da
  manobra.

A faixa de ícones aparece embaixo da faixa verde, a partir de max(400 m, 15 s) da manobra:
- as faixas que servem em azul, com a seta da manobra;
- as outras em cinza, com a seta da própria marcação;
- desenhadas com o rasterizador inteiro (`bgn_icon`), uma vez por manobra.

O aviso de radar ou lombada desceu para não encostar nas faixas.

## Verificado

- `tests/test_bglanes.c`:
  - cruzamento certo (e não o da pista contrária, nem o de lado nem um a 93 m);
  - esquerda, direita e frente;
  - rotatória sem conselho;
  - retorno sem faixa de retorno;
  - bifurcação com e sem a saída marcada.
- Dados reais do DF decodificados (por exemplo "frente | frente | frente;levemente à direita",
  chegando a 198°).
- Desenho: captura `42k` (exemplo de 4 faixas, virar à direita).
- **Ainda não visto numa manobra real:** a rota de demonstração não passa por nenhum dos 162
  cruzamentos do DF. Falta testar no FOSTON numa rota que passe.
