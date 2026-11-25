# Build e instalação

Nada do navegador entra no initrd (que está perto do limite de ~5,9 MB da NAND): o motor,
o baixador, as bibliotecas e as fontes ficam no cartão, em `BigFoston/web/` (~14 MB).
O shell (tela Internet) é parte do firmware normal.

## 1. Bibliotecas (uma vez)

Saída Buildroot separada, que usa o toolchain do firmware como externo (mesmo GCC 14.3,
mesmo musl, mesma ABI), sem recompilar o toolchain:

```sh
cd firmware/build/buildroot-2026.02.3
make O=../out-web BR2_EXTERNAL=../../buildroot bigfoston_web_defconfig
make O=../out-web
make O=../out-web host-vim host-gperf host-bison host-flex host-pkgconf
```

Gera libcurl 8 + OpenSSL 3, libpng, libjpeg-turbo, webp, freetype, DejaVu e os
certificados (ca-certificates).

## 2. Motor e baixador

```sh
tools/bigfoston/build-web.sh arm     # firmware/build/web/arm/dist  (FOSTON)
tools/bigfoston/build-web.sh host    # firmware/build/web/host/dist (PC, simulador)
```

Confere o SHA-512 do `netsurf-all-3.11.tar.gz` (em `firmware/build/dl/netsurf/`), compila
as bibliotecas do NetSurf, o `bfweb` (frontend `src/web/frontend`) e o `bfdl`, e monta
`dist/`: `bfweb`, `bfdl`, `lib/` (RPATH `$ORIGIN/lib`), `res/` (CSS, Messages, Choices,
fontes, `ca-bundle.crt`).

Detalhes do build do NetSurf que o script resolve: o frontend `bigfoston` é incluído na
lista de alvos válidos do `Makefile.hts`; o compilador do Buildroot é chamado por scripts
pequenos (o buildsystem do NetSurf usa `realpath` e o wrapper do Buildroot precisa do
próprio nome); as bibliotecas do NetSurf são instaladas na staging do `out-web`
(`PREFIX=/usr`, `DESTDIR=sysroot`).

## 3. Cartão

Pela rede USB, conferindo o SHA-256 lido do cartão e trocando a pasta inteira de uma vez:

```sh
cd firmware/build/web/arm/dist
tar cf - . | ssh root@192.168.7.2 'mkdir -p /mnt/sd/BigFoston/web.new && cd /mnt/sd/BigFoston/web.new && tar xf - && sync'
ssh root@192.168.7.2 'rm -rf /mnt/sd/BigFoston/web && mv /mnt/sd/BigFoston/web.new /mnt/sd/BigFoston/web && sync'
```

(Ou copie `dist/` para `BigFoston/web/` com um leitor de cartão.)

## 4. Shell

Como qualquer mudança no shell: `make O=../out bigfoston-shell-dirclean && make O=../out`
no Buildroot e `tools/bigfoston/linux-update.py stage`, depois desligar e ligar.
