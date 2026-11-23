# YouTube

Estado: **não testado no aparelho ainda** (falta internet no FOSTON durante o trabalho).
O que se sabe sem testar:

- **Navegador local (NetSurf)**: o YouTube depende de JavaScript moderno, Media Source
  Extensions e do elemento `<video>`. O NetSurf não toca vídeo (`NETSURF_USE_VIDEO := NO`;
  o suporte do NetSurf a vídeo é experimental) e o Duktape não executa o JavaScript do
  YouTube. Esperado: a página não funciona. Será registrado quando testado.
- **Navegador remoto** (fase 6): Chromium no PC, FOSTON como tela. Serve para navegar no
  site; vídeo pela tela remota depende da taxa de quadros que a rede USB e a decodificação
  aguentarem — a medir.
- **Player do aparelho**: a TV já decodifica H.264 em resolução reduzida (~13,7 quadros/s
  a 320×180, `docs/tv`). Um vídeo H.264 Baseline baixado de forma legítima pode ser
  reaproveitado por esse caminho no futuro, sem decodificador novo.

Não serão implementados meios de contornar as regras do serviço ou proteções de conteúdo.
