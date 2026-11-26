# Rota percorrida em tom mais claro

Durante a navegação, o trecho já percorrido é desenhado em azul-acinzentado claro
(`#9fbbe6`, ou `#5b6f8f` à noite), e o que falta em azul forte (`#1a73e8` / `#4c8bf5`).
O corte fica exatamente na posição animada do carro (`MO.s` de `bgmotion`), e não na
última posição do GPS.

O mapa não é redesenhado só por causa do progresso. A interface atualiza o ponto de corte
(`R.route_k`, `R.route_px/py`) a cada passo da animação, e o motor o usa no próximo quadro que
já ia desenhar. Como o corte fica sob o marcador do carro, o atraso não aparece.
