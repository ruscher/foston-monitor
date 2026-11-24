# Limitações conhecidas

- **Espaço da memória interna:** 12 MB na `\ResidentFlash`; zImage + initrd em 5,81 MB. Componentes
  grandes novos (obexd para capa e agenda, por exemplo) precisam ir para o cartão SD.
- **Capa do álbum:** precisa do AVRCP Cover Art (BIP, via obexd); o celular não manda ImgHandle.
- **Aleatório/repetir:** dependem do app do celular aceitar (o de teste recusa).
- **CPU:** com música, ~50 % livre; a decodificação SBC (sem FPU) é o maior custo (~21 %).
- **Conexão iniciada pelo celular:** não testada depois da correção da troca de papel.
- **Próximas fases, não implementadas:** telefone (HFP), agenda (PBAP, precisa do obexd),
  microfone (caminho de voz do DSP do MT3351 ainda não estudado no Linux). Joystick e teclado
  (HID) e internet pelo celular (PAN) já funcionam.
