# Grupos de apps — persistência

## Arquivo

`<dados>/appgroups.conf` (no aparelho, `/data/bigfoston/appgroups.conf`, no cartão SD, ao
lado de `settings.conf` e dos pacotes instalados). Texto UTF-8, uma linha por registro,
campos separados por TAB (TAB e quebra de linha nunca entram num nome: viram espaço):

```
bigfoston-apps	1
next	4
group	1	games	000000	-	
group	3	-	3584e4	tv	Diversão
item	0	sys:music
item	0	grp:3
item	3	sys:tv
item	0	sys:bluetooth
...
item	0	grp:1
item	1	pkg:sc-controller
item	0	web:https://www.biglinux.com.br/
end
```

- `bigfoston-apps <versão>`: cabeçalho; versão 1. Arquivo de versão maior é recusado (não é
  sobrescrito: vira `.bad`, ver abaixo).
- `next`: próximo id de grupo (ids nunca são reaproveitados).
- `group <id> <key|-> <cor hex|000000 = padrão> <ícone|-> <nome>`: o nome é o último campo;
  vazio = nome da categoria traduzido.
- `item <pai> <id>`: na ordem da tela. Pai 0 = tela Apps; `grp:<n>` marca a posição da pasta.
- `end`: prova que o arquivo está inteiro.

Linhas desconhecidas são ignoradas (uma versão futura pode acrescentar registros sem quebrar
esta). Por que texto e não JSON: o resto dos dados do shell já é texto simples
(`settings.conf`, `webapps.tsv`, `fm.conf`), dá para ler e consertar pelo SSH, e o leitor
cabe em poucas linhas de C sem biblioteca.

## Gravação atômica

1. escreve `appgroups.conf.tmp`;
2. `fflush` + `fsync` (no cartão FAT o conteúdo vai ao cartão antes da troca de nome — mesmo
   cuidado de `settings.conf`);
3. `rename(conf → conf.bak)` — a versão anterior vira a cópia de segurança;
4. `rename(tmp → conf)`; se falhar, `conf.bak` volta para `conf`.

Uma queda de energia em qualquer ponto deixa `conf` inteiro, ou nenhum `conf` e um `.bak`
inteiro.

## Leitura e recuperação

1. lê `conf`; se estiver inteiro e válido, usa;
2. se existir mas estiver ilegível (sem cabeçalho, versão maior, sem `end`), renomeia para
   `conf.bad` (fica para diagnóstico e nunca é sobrescrito pela próxima gravação) e
3. lê `conf.bak`;
4. sem nenhum dos dois: começa do zero (migração, ver arquitetura).

Ao ler, corrige o que não faz sentido em vez de recusar tudo: grupo repetido (vale o
primeiro), app repetido (vale o primeiro), pai que não existe (vai para a tela Apps), pasta
dentro de pasta (vai para a tela Apps), pasta de grupo inexistente (ignorada), grupo sem
pasta (pasta no fim), cor ou ícone inválidos (padrão), `next` menor que o maior id
(corrigido), nome de grupo sem nada válido (o número do grupo).

## Operações sem meio-termo

Cada operação (criar grupo + mover o app, enviar, excluir, renomear, ordenar...) faz uma
cópia do estado, aplica na memória e grava. Se a gravação falhar (cartão protegido, cheio ou
com defeito — o cartão original de 2 GB deste FOSTON já mostrou falhas de escrita), o
estado volta à cópia: nem grupo criado sem o app, nem app movido pela metade. O usuário vê
"Não foi possível salvar no cartão. Nada foi alterado."

A sincronização de apps novos é a exceção: se não conseguir gravar, fica na memória (para
o app aparecer) e tenta de novo na próxima mudança.

## Escritas na flash

Grava só quando algo muda: uma escrita por ação do usuário e uma quando aparece um app
novo. Abrir a grade, trocar de tema ou de página não escreve nada. O arquivo tem centenas de
bytes (o do aparelho, com 15 apps do sistema, 2 pacotes e 2 WebApps: 456 bytes).

## Apps removidos e instalados

- Desinstalar um pacote ou remover um WebApp apaga a entrada (`ag_forget`).
- Um app que não está presente agora (cartão fora, pacote ilegível) mantém a entrada e fica
  escondido; volta ao mesmo lugar e ao mesmo grupo.
- Reinstalar um pacote desinstalado: vai de novo para o grupo da categoria.
- Um grupo nunca some sozinho por ficar vazio.

## Compatibilidade

- O shell antigo ignora `appgroups.conf`; o novo cria o arquivo na primeira vez com a mesma
  ordem da grade antiga.
- O FIFO de controle mudou: `group N` agora é o **id** do grupo; `group games` abre o grupo
  da categoria Jogos (antes `group 0`).
- Os atalhos do Painel / apps do Quiosque continuam em `settings.conf` (`home_apps`),
  independentes dos grupos.
