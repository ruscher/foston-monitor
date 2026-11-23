# YouTube

Estado (23/11/2026): **funciona no modo completo** — busca, página do vídeo e o vídeo
tocando com som (imagem até ~12 quadros/s; som 32 kHz estéreo, ~0,3 s de atraso; shell a
~61 % de CPU). No modo leve não.

- **Navegador local (NetSurf)**: o YouTube depende de JavaScript moderno, Media Source
  Extensions e do elemento `<video>`. O NetSurf não toca vídeo (`NETSURF_USE_VIDEO := NO`;
  o suporte do NetSurf a vídeo é experimental) e o Duktape não executa o JavaScript do
  YouTube: a página não funciona no modo leve.
- **Modo completo** ([10](10-remote-browser.md)): Chrome no PC, FOSTON como tela — testado
  no aparelho: busca, canal e vídeo tocando com som (3 min seguidos sem corte, inclusive
  rolando a página). O próprio player do YouTube às vezes mostra "Algo deu errado" no meio
  de um anúncio; recarregar resolve.
- **Player do aparelho**: a TV já decodifica H.264 em resolução reduzida (~13,7 quadros/s
  a 320×180, `docs/tv`). Um vídeo H.264 Baseline baixado de forma legítima pode ser
  reaproveitado por esse caminho no futuro, sem decodificador novo.

Não serão implementados meios de contornar as regras do serviço ou proteções de conteúdo.
