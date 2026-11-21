# Quadros por segundo e roteiro de teste

## Como medir

```
# no aparelho (o shell instalado é parado e volta no fim)
scp -O src/ui/build/target/bigfoston-shell root@192.168.7.2:/tmp/shell-perf
BIGFOSTON_DATA=/data/bigfoston BIGFOSTON_BENCH=/tmp/bench.txt /tmp/shell-perf --open map
# variáveis: BIGFOSTON_BENCH_AT=lat,lon,zoom  BIGFOSTON_MAP_DETAIL=0..3 (auto, eco, equil., det.)
#            BIGFOSTON_MAP_NICE=n  BIGFOSTON_PROF=/tmp/prof.txt (perfil da fase "arrasto-so-lvgl")
```

Fases (passo de 50 ms): parado 5 s, arrasto 4 s (12 px por passo, como um dedo), assentar,
arrasto só do LVGL (a imagem anda sem pedir quadro ao motor), zoom (+1, +1, −1, −1),
rotação (6° a cada 100 ms), seguir (GPS simulado a 50 km/h, 1 Hz, mapa no rumo),
navegação (rota de demonstração simulada). Posições simuladas, nunca a do usuário.

## Taxa sustentável

O LVGL só desenha quando algo muda (o timer de desenho pausa sozinho). Em movimento contínuo
o piso é ~40 ms por quadro de tela inteira (banda de memória, [08](08-lvgl-optimization.md)):
~17-19 quadros/s no arrasto. Na navegação, com 1 posição GPS por segundo, a tela redesenha
quando o quadro do motor chega (50-170 ms no econômico) e a interface fica livre no resto do
tempo (laço p95 2 ms). Não há limite artificial de quadros: menos quadros não economizariam
nada quando nada muda, e no arrasto a taxa já é limitada pela memória.
