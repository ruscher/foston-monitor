# Entrada AV-IN

Mapa por análise estática (avin.exe, msdk.dll, sensor.dll, buses.dll, wavedev_drv.dll); ainda
não testado no aparelho.

| Parte | Achado |
|---|---|
| Aplicativo | `\Windows\avin.exe` (projeto "PlayCam_TVP5150") só fala com o driver `MDK1:` (msdk.dll) por um ioctl (`0x88000408`: abrir, configurar, iniciar, fechar, imagem) |
| Decodificador | TI **TVP5150**, I2C 0x5D (IDs 0x80/0x81 = 0x51/0x50); entrada composta AIP1A; padrão automático; sinal presente: reg 0x88 & 0x0E == 0x0E; padrão: 0x8C & 0x0F; imagem: 0x09 brilho, 0x0A saturação, 0x0B matiz, 0x0C contraste |
| Energia | GPIO48 = 1 liga o decodificador; GPIO47 reset (1 → 0 → 50 ms → 1); clocks ids 32 e 1; LDOs em 0x8006135C e 0x80061368 |
| Detecção de cabo | GPIO13 (1 = conectado), lido a cada 500 ms |
| Captura | ISP 0x80046000 (IRQ 27), um campo por vez (720×240 NTSC / 720×288 PAL) |
| Exibição | escalador CRZ 0x80049000 (720×240 → 800×480) → IPP/PRZ → DMA de imagem 0x80045000 gravando RGB565 direto no framebuffer (0x03E00030); sem camada de overlay |
| Áudio | entrada analógica de FM: `0x80061204 = 0x1480` e amplificador (GPIO5) |
| Câmera de ré | não há sinal de marcha à ré; só detecção de cabo e de sinal |

Próximo passo: ler os registradores desses blocos com o AV-IN ligado no Windows CE (`bfagent phys`)
para ter os valores de partida, e então reproduzir no Linux.
