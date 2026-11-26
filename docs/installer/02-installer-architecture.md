# 02 — Arquitetura do instalador (Fase 1: núcleo)

```
installer/
  bigfoston_installer/
    core/          sem interface, sem plataforma
      manifest.py    manifesto da versão (format 1) e lista de aparelhos (data/devices.json)
      package.py     abrir pasta/.zip de versão com lista fechada de nomes; SHA-256 por streaming
      storage.py     Card, regras de segurança (blockers/warnings), interface Storage
      install.py     preparo transacional do cartão, estado do cartão, conferência
    platforms/
      linux_udisks.py  Storage pelo UDisks2 (D-Bus), como usuário comum
    data/devices.json
  tools/make_release.py   monta BigFoston-OS-FS707DC-<versão>/ (+ .zip) a partir do build
  tests/                  pytest: núcleo (falso) + integração UDisks2 (disco de loop FAT32)
```

## Pacote de versão

`BigFoston-OS-FS707DC-<versão>/`: `manifest.json`, `SHA256SUMS`, `bfagent.exe`,
`linux/zImage`, `linux/initrd.img`, `linux/fs707dc.dtb`. O `make_release.py`:

- tira a versão do sistema de `src/ui/Makefile` e a do agente de `bfagent.c`;
- confere o `SHA256SUMS` do build (kernel, initrd e DTB do mesmo build);
- **recusa um build de outra versão**: o shell dentro da imagem precisa conter
  "BigFoston OS <versão>" (pegou de verdade um build 0.5.0 com o Makefile já em 0.5.1);
- reabre o pacote com o leitor do instalador e confere tudo.

O leitor (`package.py`) aceita só os 6 nomes conhecidos (um nível de pasta no topo do .zip),
recusa links simbólicos, `..`, arquivos extras, tamanhos acima do que o agente aceita e
.zip que mente o tamanho; o manifesto e o `SHA256SUMS` precisam dizer o mesmo.

## Regras do cartão (`storage.blockers`)

| Bloqueia | Por quê |
|---|---|
| disco com `/`, `/boot`, `/home`, `/usr`, `/var`... | é o sistema do computador |
| disco fixo (não removível, nem USB/SD) | não é um cartão |
| não FAT | o CE do FOSTON só lê FAT |
| acima de 32 GB | SDXC/exFAT nunca testados; testados: SD 2 GB e SanDisk SDHC 32 GB (`docs/sd-controller.md`) |
| somente leitura | trava do cartão / leitor |

O FOSTON pelo próprio cabo (USB `045e`) é marcado: a interface avisa que arquivos grandes
por esse caminho não são confiáveis. A identidade (série/modelo do leitor, UUID do FAT e
tamanho) é conferida de novo antes de gravar e depois de cada remontagem.

## Preparo do cartão (`install.Installer`)

1. confere o cartão e o espaço (arquivos + 4 MB);
2. grava `BigFoston/update.tmp/*` e `BigFoston/bfagent.exe.new` (fsync em cada um);
3. `sync`, desmonta, monta de novo (a releitura vem do cartão, não do cache) e confere
   cada arquivo por SHA-256;
4. troca os nomes: `update.tmp` → `update` (com `SHA256SUMS` por último, no formato do
   agente), `bfagent.exe` → `.old`, `.new` → `bfagent.exe`, grava `install.json`;
5. remonta e confere de novo com os nomes definitivos.

Cancelar só antes do passo 4; uma falha antes dele deixa o cartão como estava. Nada fora
de `BigFoston/` é tocado (mapas, iGO, `BigFoston/data` do shell ficam).

## Testes (25/11/2026)

`python3 -m pytest installer/tests`: **32/32**. Os 2 de integração (UDisks2 com uma imagem
FAT32 num disco de loop) rodam com `BIGFOSTON_TEST_POLKIT=1` (o polkit pode pedir a senha;
sem a variável eles são pulados). Cobrem: manifesto inválido (9 casos), SHA256SUMS divergente,
arquivo adulterado, link simbólico, .zip com caminho de fuga/arquivo extra/link, regras do
cartão, preparo completo preservando os arquivos do usuário, cartão que corrompe na
releitura (nada trocado), cancelamento (limpo), atualização pendente substituída, cartão
removido, outro cartão inserido, falta de espaço, conferência que acha dano.

## Corrigido

- `tools/bigfoston/sd-install.sh` copiava `initrd.gz`, que o build não gera mais: agora
  `initrd.img` (o nome do agente, do `linux-update.py` e do `SHA256SUMS` do build).

## Pacote da versão (`installer/tools/make_release.py`)

- **O DTB vai com `/chosen/bigfoston,boot-epoch`** (o mesmo `dtb_with_epoch` do
  `linux-update.py`). Sem esse campo, o agente recusa a atualização na partida ("RECUSADA:
  fs707dc.dtb sem /chosen/bigfoston,boot-epoch"). Isso aconteceu em 26/11/2026 com uma cópia
  feita à mão.
- **Recusa um initrd com as chaves SSH do construtor** (`root/.ssh/authorized_keys` ou a chave
  de host fixa do dropbear): no pacote público, elas dariam acesso root a todo FOSTON instalado.
  O pacote público sai de um build feito assim:

  ```
  BIGFOSTON_SSH_PUBKEY=/dev/null/x BIGFOSTON_SSH_HOSTKEY=/dev/null/x make
  ```

  Sem chave, o SSH fica fechado e o dropbear cria a própria chave de host no primeiro uso.
