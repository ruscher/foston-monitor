# Comparação de motores

Critério: rodar **no FOSTON real** (ARMv6 sem NEON, soft-float, 128 MB, sem GPU, rootfs
musl, initrd no limite de ~5,9 MB) fazendo algo útil sem derrubar o GPS.

| Motor | ARMv6 / sem GPU | Evidência | Decisão |
|---|---|---|---|
| **Blink** (Chromium) | não | O Chromium só tem builds e configuração para ARMv7 ou mais novo; precisa de ~centenas de MB de RAM por processo de renderização, V8 e multiprocesso. Não compilado aqui. | descartado |
| **Gecko** (Firefox) | não | O Firefox para Linux ARM exige ARMv7-A; o SpiderMonkey precisa de mais RAM do que o aparelho tem livre. Não compilado aqui. | descartado |
| **WPE WebKit** 2.50.5 | não | No Buildroot do projeto, `package/wpewebkit/Config.in` exige `BR2_PACKAGE_HAS_LIBGLES` e `BR2_PACKAGE_HAS_LIBEGL` (OpenGL ES/EGL), além de C++, GCC ≥ 11, GLib, ICU, HarfBuzz, libsoup3 e GStreamer. O MT3351 não tem GPU: só restaria GL por software (Mesa), lento demais a 468 MHz. | descartado para uso local |
| **WebKitGTK** | não | Mesmo motor, mais GTK e X11/Wayland. | descartado |
| **NetSurf 3.11** | **sim** | Compilado para o aparelho com o toolchain do firmware; **roda no FOSTON** (ver [26](26-benchmarks.md)): página de teste em 0,74 s com 12 MB de RSS. HTML 4/5, CSS 2.1 e parte do CSS3, JavaScript limitado (Duktape). | **motor padrão** |
| Dillo 3.2.0 | parcial | No Buildroot exige Xorg e C++ (FLTK). Sem JavaScript. | descartado (precisaria de X) |
| Links 2.30 (gráfico) | parcial | Modo gráfico exige GPM ou Xorg; sem JavaScript, CSS mínimo. | descartado |

## Por que NetSurf, e como

- É o único que roda **localmente** com HTML/CSS de verdade dentro de 128 MB.
- O libnsfb 3.11 **não tem mais a superfície framebuffer do Linux** (só ram, sdl, vnc, wld,
  x): o frontend framebuffer pronto não roda sozinho no `/dev/fb0`. Por isso o BigFoston
  tem um frontend próprio (`src/web/frontend`) que desenha numa superfície `ram` RGB565 em
  memória compartilhada; a interface (barra, teclado, menus) é do próprio shell LVGL.
- Versão: 3.11 (dez/2023, a última publicada; o Buildroot ainda traz a 3.10 de 2020).
  O tarball `netsurf-all-3.11.tar.gz` é conferido pelo SHA-512 (o mesmo do Alpine Linux;
  o site do NetSurf não publica soma).

## Sites pesados

Para sites que dependem de JavaScript moderno (YouTube, redes sociais, webmail), a saída
planejada é o **navegador remoto**: Chromium no PC e o FOSTON como tela e toque (fase 6,
ainda não feita). O WPE pode voltar como experimento só se aparecer um caminho sem GL.
