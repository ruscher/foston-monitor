# Limitações conhecidas

- **JavaScript**: Duktape (ES5 com partes do ES2015), sem JIT, lento no ARMv6; desligado
  por padrão (menu → JavaScript). Sites que dependem dele não funcionam; para eles, o
  navegador remoto (fase 6, não feito).
- **Vídeo e DRM**: sem `<video>`, sem EME/Widevine ([19](19-youtube.md), [20](20-streaming-drm.md)).
- **Páginas grandes**: um artigo de 1,2 MB de HTML usa ~40 MB no motor; a memória só volta
  ao fechar o app ([26](26-benchmarks.md)). O limite de 88 MB protege o resto do sistema.
- **Uma janela**: links `target=_blank` abrem na mesma janela.
- **Navegação privada**: não existe como modo; os cookies já ficam só na RAM e o histórico
  pode ser desligado — não é anunciado como "privado".
- **Senhas**: não são guardadas.
- **Páginas internas do NetSurf** (erros, certificado) ainda em inglês.
- **Download de POST**: não pode ser repetido pelo `bfdl`.
- **WebApps**: por enquanto aparecem na página inicial do Internet, não na grade da Início
  nem no Foston Glass. São atalhos: precisam de internet e abrem no navegador.
- **Toque**: sem pinça (tela resistiva de um ponto); zoom pelo menu.
- **Seleção de texto e colar**: ainda não.
- **Sem internet própria**: depende do PC (rede USB com compartilhamento) ou, no futuro,
  do celular por Bluetooth PAN.
