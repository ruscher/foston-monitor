# Etapa 6 — Testes e medidas

## Testes de unidade (`make test`, com ASan e UBSan, no PC)

| Teste | Verificações | O que cobre |
|---|---|---|
| `tests/test_padmap.c` | 32 | GUID; ler e escrever a linha; metade e inversão de eixo; numeração do SDL (DS4 sem driver, joystick com teclas); mapa padrão do Linux; busca (exato, versão zerada, o banco do usuário vence, outra versão do driver não serve) |
| `tests/test_pkg.c` | 22 | ids; info; nome no idioma; instalar; atualizar com gzip; sem sobras; recusa `..`, caminho absoluto, link, duas pastas, id inválido, sem `.desktop` e lixo; nada escapa da pasta; desinstalar |
| `tests/test_kbdmap.c` | 33 | layouts do teclado (anteriores) |

Os pacotes do `test_pkg.c` são montados no próprio teste (ustar e gzip pelo lodepng).

## Simulador (`--screenshot`, com `BIGFOSTON_TEST_BFAPP=pkg/apps/sc-controller.bfapp`)

| Captura | Verifica |
|---|---|
| 44a-jogos-vazio | Jogos existe sem apps |
| 44b-instalar | "Instalar SC Controller?" em português, grupo e tamanho |
| 44c-instalado | "SC Controller instalado — Está em Apps > Jogos" |
| 44d-apps-pasta-jogos | a pasta Jogos na grade, com selo "1" |
| 44e-grupo-jogos | a tela do grupo com o app |
| 44f-scc-controles | o controle, o mapa "banco do SC Controller" e o GUID |
| 44g-scc-testar | botões acesos, analógicos, gatilho R2 e evento cru |
| 44h–44j | registro: introdução, etapa "1 de 21", fim |

Mais 12 verificações automáticas:
- instalado em Jogos, com ícone;
- o mapa passa a vir do banco;
- abre pelo `.desktop`;
- no testador o B não navega, e segurar B sai;
- o registro sai igual ao banco do SDL e passa a valer;
- esquecer volta ao banco;
- desinstalar volta ao mapa embutido.

Com o ASan, as 150 capturas não tiveram nenhum erro de memória.

## Aparelho (24/11/2026), com toques de verdade na tela e um DS4 virtual (`uinput`)

1. Jogos vazio → **Abrir Arquivos** → `BigFoston/Apps` → `sc-controller.bfapp` (ícone de
   download) → "Instalar?" em português → **Instalar** → "instalado, Está em Apps > Jogos".
2. Apps, página 2: pasta **Jogos** com o selo "1" → grupo → **SC Controller**.
3. Controles: o controle virtual com o mapa "banco do SC Controller"; a lista se atualizou
   sozinha quando ele conectou.
4. Testar: ✕, R1, direcional e analógicos acesos; o Bola não saiu do teste; **segurar o Bola
   2 s** saiu.
5. Registrar: as 21 etapas → Salvar → a linha gravada é **igual** à do banco (comparação
   campo a campo) → **Esquecer registro** → de volta ao banco.
6. PS levou ao Início (o controle volta a mexer na interface fora da captura).
7. Segurar o ícone → "Desinstalar?" → desinstalado (aviso; mapa de volta ao embutido) →
   `bfapp.py install` pelo PC → instalado de novo (aviso "SC Controller instalado — Jogos").

Do uso real com o DualShock 4 de verdade (registro do aparelho):
- depois de desligar por inatividade, o controle **reconectou sozinho pelo botão PS** três
  vezes (05:29, 05:30, 05:43);
- o desligamento depois de 10 min parado funcionou.

## Correção do DualShock 4 real (24/11/2026)

Os testes acima usaram um DS4 **virtual** (`uinput`), que entrega eventos prontos. O DualShock
real mandava o relatório Bluetooth 0x11, que o descritor do SDP não descrevia, e **nenhum
botão chegava**. A causa e a correção estão em
[03-mapa-de-controles.md](03-mapa-de-controles.md#dualshock-4-pelo-bluetooth-o-relatório-0x11-24112026).

Agora o teste no aparelho também confere a camada de baixo. Com o controle real conectado:
```
cat /dev/input/eventN | wc -c        # aperte botões: precisa crescer (16 bytes por evento)
cmp /sys/class/input/eventN/device/device/report_descriptor <descritor esperado>
```
