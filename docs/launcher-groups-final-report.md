# Grupos de apps — relatório final

## O que mudou para o usuário

- Segurar um app por **2 segundos** (um anel mostra a contagem) abre o menu: Abrir, Criar
  novo grupo, Enviar para o grupo, Mover aplicativo e, quando cabe, Remover do grupo,
  Renomear grupo, Desinstalar.
- **Criar novo grupo** pede o nome (teclado com acentos) e já leva o app para dentro; a pasta
  aparece onde o app estava.
- Segurar uma **pasta**: Abrir, Renomear, Cor e ícone, Mover pasta, Ordenar por nome,
  Excluir. Excluir só desfaz a pasta: os apps voltam para a tela Apps, nada é desinstalado.
- **Mover**: tocar no novo lugar, ou numa pasta para pôr dentro — o "arrastar" pensado para
  o toque resistivo.
- **Busca** pela lupa da tela Apps.
- **Painel** ganhou o ícone **Apps** com todos os apps e grupos.
- A organização é **uma só para o sistema**: Grade, Painel, Foston Glass mostram os mesmos
  grupos; vale depois de reiniciar.

## Como foi feito

Uma camada única (`core/appgroups.c`) guarda e grava a organização por IDs estáveis; uma
ponte (`core/launcher.c`) resolve cada ID; os fluxos de interface (`screens/launcher_ui.c`)
são os mesmos em todos os temas e cada tema só muda a pele. Detalhes em
[arquitetura](launcher-groups-architecture.md), [UX/UI](launcher-groups-ux-ui.md) e
[persistência](launcher-groups-persistence.md).

A categoria **Jogos** (e as outras categorias dos pacotes) virou um grupo comum com uma
`key`: aparece igual a antes, pode ser renomeada, recolorida, reordenada ou excluída, e os
jogos instalados depois continuam indo para ela.

## Testes

Unidade 64/64, interface no simulador 55/55 (toque de verdade via ponteiro do LVGL, também
com ASan/UBSan), suíte inteira sem falhas, e o fluxo principal no FOSTON real: toque longo,
criar com acento, enviar, remover, excluir, Painel > Apps, Glass, busca, mover, reinício do
processo e carga com 41 pastas. Tabela completa em [testes](launcher-groups-tests.md).

## Decisões

- **Sem arrastar de verdade**: num resistivo de um ponto, numa grade paginada em que o
  deslizar já troca de página e o dedo parado abre o menu, o arrastar disputaria o mesmo
  gesto. O modo mover faz tudo que o arrastar faria, com toques simples.
- **Quiosque sem menu de grupos**: é o modo travado nos apps escolhidos.
- **Atalhos do Painel independentes**: continuam sendo os "favoritos" de Aparência; um app
  num grupo continua no atalho.
- **Soltar entre 0,7 s e 2 s não abre o app**: quem segurou e viu o anel estava pedindo o
  menu; abrir o app seria surpresa.
- **Confirmação discreta** no rodapé, em vez do aviso grande do centro da tela.

## Pendências e limites

- **Desligar e ligar** o aparelho com grupos criados não foi testado (o reinício remoto foi
  bloqueado nesta sessão); a persistência foi conferida reiniciando o processo do shell, que
  relê o arquivo do cartão. Vale um teste com o botão de energia.
- **Mover entre dois grupos** e **renomear** foram testados no simulador e na unidade, não
  no aparelho.
- **Mouse** não foi exercitado (o simulador usa o mesmo tipo de entrada, mas não houve sessão
  interativa).
- O toque no aparelho foi **injetado no driver**; falta conferir com o dedo o limite de 18 px
  e o 0,7 s no resistivo.
- **Mais usados / recém-instalados** não existem: não há registro de uso nem data confiável.
- Se o usuário criar um grupo "Games" e trocar a língua para inglês, a categoria Jogos também
  se chama "Games": dois grupos com o mesmo nome exibido (os dois continuam funcionando).
- O shell não foi instalado no firmware nem a versão foi mudada; isso fica para a próxima
  atualização do BigFoston OS (`linux-update.py stage`).
