# Etapa 2 — Pacotes .bfapp

## Formato

Um `.bfapp` é um arquivo **tar ustar**, cru ou com **gzip**, com **uma pasta só**, a do app:

```
sc-controller/
sc-controller/sc-controller.desktop     o .desktop (na raiz da pasta)
sc-controller/icon.svg                  Icon=icon.svg
sc-controller/gamecontrollerdb.txt      o resto do app
sc-controller/LICENSE-gamecontrollerdb.txt
```

O nome da pasta é o **id** do app: `[a-z0-9._-]`, até 40 caracteres, sem começar com `.`
ou `-`.

## Instalar

1. O usuário copia o `.bfapp` para o cartão. A sugestão é `BigFoston/Apps`, onde o
   `bfapp.py copy` põe o pacote.
2. Em **Arquivos** o pacote aparece com o ícone de download. Tocar nele abre
   **"Instalar SC Controller?"**, com o comentário no idioma do sistema, o grupo e o
   tamanho.
3. Ao tocar em **Instalar**, o shell faz:
   - extrai em `<dados>/apps/.<id>.novo`;
   - troca a versão anterior, se houver: ela vira `.<id>.velho` e só é apagada depois de a
     nova estar inteira no lugar;
   - roda `sync`.
4. Depois aparece **"SC Controller instalado — Está em Apps > Jogos"**, com os botões OK e
   **Abrir**.

Instalar um pacote já instalado é **atualizar**: o diálogo diz "Atualizar SC Controller?".

Depois de instalar ou desinstalar, o shell relê os apps, refaz os ícones, procura de novo
os mapas dos controles (o app pode trazer um `gamecontrollerdb.txt`) e redesenha a
grade ou o grupo à vista.

## Desinstalar

Segurar o ícone dentro do grupo abre **"Desinstalar SC Controller?"**, com o botão vermelho
Desinstalar. Ele apaga `<dados>/apps/<id>` e mostra o aviso "SC Controller desinstalado".

## Segurança (`src/ui/core/pkg.c`)

O pacote vem de fora e o shell roda como root. O leitor de tar é próprio e rígido:
- cabeçalhos com checksum conferido;
- só arquivos comuns e pastas: link simbólico, link físico, aparelho, FIFO e
  "nome longo" do GNU são **recusados**;
- caminho absoluto, `..`, `.`, partes vazias, `\` e caracteres de controle são **recusados**;
- tudo dentro da pasta do app: uma segunda pasta ou um id inválido são recusados;
- no máximo **512 arquivos** e **24 MB**, com o gzip limitado também na saída;
- precisa de um `.desktop` com `Name=` na raiz da pasta;
- cabeçalhos pax (`x`/`g`) são ignorados: o caminho usado é sempre o do ustar, já conferido.

Erro, cada um com uma mensagem clara (`S_PKG_E_*`):

| Erro | Mensagem |
|---|---|
| `PKG_E_READ` | O arquivo não pôde ser lido. |
| `PKG_E_FORMAT` | O arquivo não é um pacote .bfapp válido. |
| `PKG_E_UNSAFE` | O pacote foi recusado: tem caminhos ou arquivos que poderiam sair da pasta do app. |
| `PKG_E_BIG` | O pacote é grande demais (máximo 24 MB, 512 arquivos). |
| `PKG_E_NODESKTOP` | O pacote não tem o arquivo .desktop do app. |
| `PKG_E_WRITE` | Não foi possível gravar no cartão. |

## Pelo PC: `tools/bigfoston/bfapp.py`

```
bfapp.py build apps/sc-controller      # monta pkg/apps/sc-controller.bfapp
bfapp.py copy pkg/apps/sc-controller.bfapp     # cartão: BigFoston/Apps (SHA-256 conferido)
bfapp.py install pkg/apps/sc-controller.bfapp  # copia e instala (aviso no aparelho)
bfapp.py install ... --ask                     # o aparelho mostra o "Instalar?"
bfapp.py uninstall sc-controller
bfapp.py list
```

O `build` faz as conferências abaixo e monta um pacote **reproduzível**:
- confere o `.desktop` (`Name`, `Categories`, `Exec` ou `X-BigFoston-Screen`) e o ícone;
- deixa de fora o que estiver no `.bfappignore`;
- grava nomes em ordem, data zero, dono root e gzip com `mtime=0`.

Montar duas vezes deu o mesmo SHA-256.

## FIFO de controle (`/run/bigfoston/ctl`)

| Comando | Faz |
|---|---|
| `install CAMINHO` | instala sem perguntar e mostra o aviso flutuante |
| `install-ask CAMINHO` | abre o "Instalar?" |
| `uninstall ID` | desinstala |
| `group N` | abre o grupo N (0 = Jogos) |
| `app sc-controller` | abre o SC Controller |
