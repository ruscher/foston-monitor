# Visão geral da rota durante a navegação

- Na barra de baixo da navegação, o botão com o olho enquadra a rota inteira:
  - norte para cima;
  - zoom para caber a rota;
  - o acompanhamento é desligado.
- O botão de posição (seta) volta à câmera de navegação:
  - centro na posição animada do carro (`bgmotion`), não na última leitura do GPS;
  - rumo da câmera;
  - zoom de navegação (`nav_zoom`) **aos poucos**. Diferenças grandes andam 1/6 por quadro
    do motor (de 12 para 16,5 em ~6 quadros), e as pequenas 0,1 nível por quadro, sem salto.
