# Netflix, Prime Video e DRM

Resposta curta: **não funcionam no FOSTON**, e isso não é questão de otimização.

- Os dois serviços exigem Widevine (via Encrypted Media Extensions) num navegador e
  plataforma suportados por eles. O Widevine não é distribuído para Linux ARM 32 bits
  (ARMv6), não é de código aberto e não pode ser incluído no firmware.
- Nenhum motor que roda no aparelho (NetSurf) tem EME ou `<video>` com DRM.
- O navegador remoto também não resolve: retransmitir a tela de conteúdo protegido não é
  permitido pelos serviços, e o próprio navegador do PC pode exibir tela preta nessas
  situações.
- O BigFoston OS não terá nenhum mecanismo para contornar DRM.

Alternativas legítimas: assistir no celular ou no PC; um equipamento externo autorizado
na entrada de vídeo do FOSTON, se o AV-IN for concluído (respeitando HDCP — um adaptador
HDMI→AV comum não resolve conteúdo protegido).

O app Internet mostra essa limitação na tela Sobre.
