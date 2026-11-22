# Limitações

- **Banda de memória (58 MB/s):** redesenho de tela inteira custa ≥ ~40 ms; o arrasto fica
  em ~17-19 quadros/s. Desenhar direto no framebuffer economizaria uma cópia (~13 ms), mas
  exige ler de memória *write-combined* e mostraria o quadro sendo montado: não feito.
- **Quadro do motor em cidade densa:** 95-130 ms no econômico em z13-z15; com o mapa no rumo
  (rotação a cada mudança de direção) não dá para deslocar o quadro anterior, então o desenho
  incremental (só a faixa nova) ajudaria pouco na navegação e não foi feito.
- **Mapa pré-rasterizado no PC:** ocuparia dezenas de GB para o Brasil em z12-z17 e perderia
  a rotação e o modo noturno; o vetorial continua.
- **GPS:** ~4 % de CPU em repouso (leitura serial imitando o WinCE + motor MTK); não mexido
  para não arriscar o GPS que funciona.
- **Vigia do cartão:** `bigfoston-sd watch` checa a cada 1 s (`sleep 1` cria um processo);
  custa 2-4 % de CPU parado. Um uevent do kernel evitaria isso; ainda não feito.
- **Não medido:** latência física do toque, navegação real em movimento, uso por horas.
