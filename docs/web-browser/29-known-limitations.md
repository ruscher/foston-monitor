# Limitações conhecidas

- **Modo performance**: enquanto ligado, o FOSTON não recebe chamadas nem usa GPS ou
  Bluetooth (volta 10 s depois de o vídeo parar); desligável no menu do Internet.
- **Tela cheia**: sem sincronismo vertical, uma troca rápida de cena pode mostrar uma emenda
  horizontal por um quadro.
- **Modo completo**: precisa do `bfweb-pc.py` rodando no PC; vídeo a ~15–20 quadros/s; o som
  chega ~0,3 s atrás da imagem do Chrome e precisa de PulseAudio/PipeWire no PC (`pactl` e
  `parec`); downloads completos só depois de baixados no PC (não dá para pausar).
- **Modo leve — JavaScript**: Duktape (ES5 com partes do ES2015), sem JIT, lento no ARMv6; desligado
  por padrão (menu → JavaScript). Sites que dependem dele não funcionam; para eles, o
  modo completo pelo PC ([10](10-remote-browser.md)).
- **Vídeo e DRM**: sem `<video>`, sem EME/Widevine ([19](19-youtube.md), [20](20-streaming-drm.md)).
- **Páginas grandes**: um artigo de 1,2 MB de HTML usa ~40 MB no motor; a memória só volta
  ao fechar o app ([26](26-benchmarks.md)). O limite de 88 MB protege o resto do sistema.
- **Uma janela**: links `target=_blank` abrem na mesma janela.
- **Navegação privada**: não existe como modo; os cookies já ficam só na RAM e o histórico
  pode ser desligado — não é anunciado como "privado".
- **Senhas**: não são guardadas.
- **Páginas internas do NetSurf**: títulos, botões e os erros de rede traduzidos
  (`res/Messages.pt`); textos raros ainda em inglês.
- **Download de POST**: não pode ser repetido pelo `bfdl`.
- **WebApps**: são atalhos (precisam de internet e abrem no Internet); não entram no modo
  Quiosque nem no dock do Foston Glass (estão na grade do botão Apps); até 24.
- **Tela cheia**: só no modo completo e só da página principal (um player dentro de
  `<iframe>` de outro site não ocupa a tela toda); o Chrome headless recusa a tela cheia
  real, então ela é emulada na página.
- **Toque**: sem pinça (tela resistiva de um ponto); zoom pelo menu.
- **Seleção de texto e colar**: ainda não.
- **Modo leve — sites modernos**: sem CSS grid e com JavaScript limitado; sites como g1,
  gov.br e o próprio biglinux.com.br ficam quebrados ou vazios (use o modo completo).
- **Modo leve — páginas pesadas**: uma página grande da Wikipédia leva ~80 s e ~60 MB no
  aparelho (ARM1176 de 468 MHz, sem NEON nem instruções de AES); páginas simples, 3–7 s
  ([26](26-benchmarks.md)).
- **Atualizar o pacote do navegador com o app aberto**: o motor em execução continua com as
  bibliotecas antigas até o app ser fechado (sair para a Início basta).
- **Sem internet própria**: depende do PC (rede USB com compartilhamento) ou, no futuro,
  do celular por Bluetooth PAN.
