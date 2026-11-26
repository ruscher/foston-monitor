# Regressões verificadas e bugs encontrados

## Testes automáticos (`make test` em `src/ui`, com ASan/UBSan, sem avisos)

webdata, lz4dec, kbdmap, padmap, pkg, appgroups (os de antes) e os novos:
- `bgmotion`: movimento entre posições, parado, sem sinal, câmera;
- `bgalert`: alertas na rota, avisos do usuário;
- `bgvoice`: ADPCM, normalização, pacote real;
- `bgnavzoom`: zoom nas manobras;
- `bgvehicle`: marcadores;
- `bgpersp`: vista inclinada;
- `bgnavlegs`: paradas, repetição entre rotas;
- `bglanes`: faixas.

Capturas do simulador (`--screenshot`, 190 telas, mapa e rotas do DF) sem nenhuma falha,
com pasta de dados nova.

## Desempenho no aparelho (mesmo roteiro do bench, antes e depois)

| Fase | 0.5.0 | Big GPS 2.0 (2D) | 2.0 com 3D |
|---|---|---|---|
| Seguir | 2,9 q/s | 18,6–19,6 | 16,1–16,7 |
| Navegação | 2,5 q/s | 13,1–15,5 | 11,7–12,3 |
| Arrasto | 18,0 q/s | 16,8–18,3 | — |
| Abrir o mapa (marcadores) | 433 ms | 39 ms | |

O arrasto ficou na mesma faixa, e a navegação e o seguir multiplicaram.

## Bugs encontrados e corrigidos

| Onde | O que | Como foi achado |
|---|---|---|
| Navegação | ~2 FPS (a causa original) | bench no aparelho |
| Animação | redesenhar a tela inteira a cada passo mesmo sem mudar 1 px (85 % de CPU) | bench |
| Bench antigo | 12,7 FPS de rotação inflados por quadros repetidos | bench |
| Fim de rota | a previsão passava do destino e voltava | teste |
| Pino | pegar perto do topo fazia o mapa correr 690 m | toque injetado no aparelho |
| Pino | o último movimento do dedo não era aplicado | toque injetado |
| Marcador | `float` sem FPU: painel travado por segundos | aparelho |
| Painel | apagar o painel dentro do evento do próprio botão | revisão |
| Abrir o mapa | 433 ms de desenho em `float` | medição |
| 3D | média > 500 ms desligava o 3D num pico de leitura do cartão | bench |
| Projeção | deslocamento de número negativo (comportamento indefinido) | UBSan |
| Janela de nome (lançador) | ponteiro pendurado após troca de tela: falha de segmentação | capturas |
| Busca | "avenda", "nrte" e "tagautinga" corrigidos errado ou não corrigidos | testes da busca |
| Chip de aviso | formato com argumento trocado | compilador (-Wformat) |
| Modo Performance | podia pausar Bluetooth e telefone com o mapa aberto | revisão |
| Compilação | avisos antigos (truncamento no BRP, `_GNU_SOURCE`) | build |

## Testado e descartado

Girar a imagem pronta entre quadros: a navegação caiu de 14,4 para 11,2 quadros/s e o toque
ficou mais lento ([04](04-arquitetura-renderizacao.md)).
