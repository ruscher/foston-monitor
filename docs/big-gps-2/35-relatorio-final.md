# Big GPS 2.0: relatório

Branch `feat/big-gps-apple-maps-experience`, a partir do `main` 0.5.1. Foram 24 commits, com
78 arquivos e cerca de 5.600 linhas. As medições são do FOSTON FS-707DC, com GPS simulado,
salvo quando indicado. Nada foi testado dirigindo.

## 1. Os 2 FPS

O GPS dá uma posição por segundo, e cada posição pedia um quadro inteiro do mapa. Com o mapa
no rumo, a imagem anterior nem era deslocada. O processador ficava 80 % ocioso. Detalhes em
[01](01-auditoria.md).

## 2–3. Arquitetura antes e depois

**Antes:** GPS → quadro inteiro do motor → tela.

**Agora:**
- o GPS alimenta o `bgmotion`, que anima a posição a 20 Hz pela geometria da rota e suaviza a
  câmera;
- a interface desliza o quadro pronto;
- o motor só desenha quando precisa (16 px fora, 2° de giro ou outro zoom);
- na vista 3D, a interface projeta a fonte.

Detalhes em [04](04-arquitetura-renderizacao.md) e [05](05-gps-interpolacao.md).

## 4–9. Números

| | Antes (0.5.0) | Depois (2D) | Depois (3D) |
|---|---|---|---|
| Navegação | 2,5 quadros/s, CPU 20 % | 13–15,5 q/s, CPU 46–53 % | 11,7–12,3 q/s, CPU 64 % |
| Seguir | 2,9 q/s | 18,6–19,6 q/s | 16,1–16,7 q/s |
| Laço da interface p95 (navegação) | 24 ms | 79–97 ms | 136 ms |
| RSS na navegação | 14,9 MB | 15,3–16,8 MB | 20,8 MB |
| Abrir o mapa | +433 ms (marcadores) | +39 ms | |

O laço da interface ficou mais lento porque agora há trabalho de verdade (animação). Ainda
não foi medida a latência do toque em si.

## 10–14. Recursos

| Recurso | Estado |
|---|---|
| 3D (2.5D) | feito, ~12 q/s, recua sozinho se ficar lento |
| Zoom nas manobras | feito, testado |
| Alertas | radares, lombadas, passagens de nível e semáforos (169 mil pontos); voz e chip; lombadas suas em azul |
| Pino arrastável | feito, testado no aparelho com toques injetados |
| Busca | já tinha abreviações; agora corrige erros de digitação |
| Paradas | até 3, testadas no aparelho |
| Alternativas | rotas reais, testadas no simulador |
| Faixas | dados do OSM; desenho testado no simulador |
| Marcadores | 9 modelos, 8 cores, 3 tamanhos, testados no aparelho |
| Painéis | rotas à esquerda e coluna de navegação, no simulador |

## 15–16. Voz Piper Faber

A única voz é a Faber, gerada no PC. São três pacotes:
- o fixo, com 129 frases (1,9 MB);
- o do DF, com 14.148 nomes de vias (596 MB, 41 min no PC);
- o do restante do Centro-Oeste (GO, MT e MS), com 66.692 nomes (2,1 GB, ~3 h no PC).

Desde a 0.6.4 só o índice dos pacotes fica mapeado e o áudio é lido do cartão ao falar
([22](22-piper-faber.md)). Ela foi ouvida no aparelho pelo usuário. Rodar o modelo
no FOSTON é inviável: cerca de 39 minutos por frase. O eSpeak NG saiu.

## 17. Bluetooth

Nada desliga o Bluetooth. Com o mapa aberto, o modo Performance não pausa nem o Bluetooth nem
o telefone. A música some durante a fala em vez de abaixar; o plano está em
[24](24-audio-bluetooth.md). **Ainda não testado com música tocando.**

## 18. Capturas

Simulador: `42b`–`42m` (`src/ui/core/screenshot.c`). No aparelho, quadros capturados de
`/dev/fb0` durante os testes: navegação, alertas, pino, marcador, 3D e paradas.

## 19–22. Arquivos, testes, bugs e regressões

- Biblioteca nova em `src/ui/biggps/`: `bgmotion`, `bgalert`, `bgvoice`, `bgvehicle`,
  `bgpersp` e `bglanes`.
- Construtores novos em `tools/big-gps/builder/`: `bgalert-build`, `bgvoice-pack`,
  `bgvoice-names` e `bglanes-build`.
- Gerador da voz: `tools/big-gps/make-faber-voice.py`.
- Testes, bugs e regressões: [31](31-regressoes.md).

## 23. Recuperação

Ver [32](32-seguranca-recuperacao.md).

## 24–25. Gerar os dados e a voz

```
tools/big-gps/build-data.sh brazil-latest.osm.pbf SAIDA brasil -74.1,-34.0,-28.5,5.4
# voz com nomes de uma região:
tools/big-gps/builder/bgvoice-names centro-oeste-latest.osm.pbf nomes.txt --bbox=-48.29,-16.06,-47.30,-15.49
tools/big-gps/make-faber-voice.py faber-df.bgv --nomes nomes.txt
```

No cartão:
- `BigFoston/big-gps/voz/*.bgv`;
- `alertas/*.bga`;
- `rotas/*.bgl`.

## Depois do relatório (26/11/2026, BigFoston OS 0.6.0)

- **Música do Bluetooth abaixa durante a fala** (-12 dB), em vez de sumir
  ([24](24-audio-bluetooth.md)). Ouvido e aprovado pelo usuário.
- **Placa do radar** com o limite e **bipe acima da velocidade**
  ([15](15-alertas-radares.md)).
- O usuário testou no aparelho: alternativas, faixas, painéis, busca e lombadas marcadas.
- **Desempenho com o GPS procurando satélites e música tocando:** pior que o medido antes
  ([30](30-benchmarks.md)). Falta medir com o GPS fixado.

- **Limite de velocidade das vias**, do `maxspeed` do OSM ([36](36-limites-de-velocidade.md)), BigFoston OS 0.6.2.

- **Paradas:** reordenar, tirar uma e arrastar no mapa ([20](20-paradas-intermediarias.md)), BigFoston OS 0.6.3.
- **Revisão visual** com paleta própria ([03](03-design-system.md)) e **voz com nomes de vias de
  todo o Centro-Oeste** ([22](22-piper-faber.md)), BigFoston OS 0.6.4.
- **Modo Performance com o mapa aberto**, testado no FOSTON (26/11/2026): pausou só as
  tarefas agendadas (`crond`); Bluetooth, GPS e telefone continuaram, com os mesmos processos.
  A etiqueta "Performance" ficava sobre o botão Voltar; agora vai para um lugar livre
  ([25](25-modo-performance.md)).

## 26. Próximos passos

- Testar dirigindo: navegação com o GPS de verdade, limite da via, placa e bipe do radar, voz
  com nomes de vias fora do DF, e com música do celular tocando.
- Medir o desempenho com o GPS fixado, no carro.
