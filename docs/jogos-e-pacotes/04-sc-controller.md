# Etapa 4 — O app SC Controller

Grupo Jogos, pacote `sc-controller.bfapp` (15,8 KB; 203 KB instalado). A tela é
`src/ui/screens/scr_scc.c`, aberta pelo `.desktop` com `X-BigFoston-Screen=sc-controller`.
Tem três abas, em botões arredondados no alto.

## Controles

Um cartão por controle conectado, com:
- o nome;
- Bluetooth (endereço) ou USB;
- o **mapa** e de onde veio: "banco do SC Controller", "embutido no BigFoston", "registrado por
  você" ou, em amarelo, "padrão do Linux: registre se algum botão estiver trocado";
- o GUID;
- os botões **Testar** e **Registrar**.

Sem controle, o cartão diz como ligar ("Já pareado? Aperte PS") ou parear, e tem o botão
Abrir Bluetooth. No rodapé, o tamanho do banco: "746 modelos para Linux e N registrados por
você".

A lista se refaz sozinha quando um controle chega ou sai: o timer confere a cada 400 ms.

## Testar

Um controle desenhado com objetos do LVGL:
- **silhueta**: o corpo e as duas pegadas;
- **botões que acendem**: ombros L1/R1 (LB/RB), direcional, Back/Start, Guia (PS) e, no
  PlayStation, o touchpad;
- **gatilhos**: barras que enchem conforme o quanto são apertados;
- **analógicos**: um anel com um ponto que anda; o anel acende ao afundar (L3/R3);
- **botões de frente**: no PlayStation, as formas △ ○ ✕ □ desenhadas com linhas e nas cores do
  console (a fonte não tem esses símbolos); nos outros, A B X Y nas cores do Xbox.

Botão aceso ganha cor, brilho e um traço que contrasta com o fundo, para o rótulo não
sumir. Embaixo aparecem:
- o último evento cru, por exemplo `EV_KEY 0x131 (b1) = 1`, com o código do evdev e o
  número do SDL (para comparar com o banco);
- a contagem de eventos;
- a dica: "Segure B (Bola) por 2 s ou toque em Controles para sair".

Enquanto o testador está aberto, o controle **não mexe na interface** (`gamepad_capture`):
o B não volta e o cursor some. Para sair pelo controle, segure **B por 2 s**. O timer roda a
30 quadros/s só nesta aba.

## Registrar (o "Device registration" do sc-controller)

Para controle sem mapa ou com botão trocado.

1. "Ensine ao BigFoston cada botão do seu controle" e o botão **Começar**. Se já houver
   registro, aparece também **Esquecer registro**.
2. São **21 etapas**, com barra de progresso, "N de 21", **Voltar** e **Pular** (para
   controle sem aquele botão):
   - A, B, X, Y;
   - LB, RB, LT, RT;
   - Back, Start, Guia;
   - L3, R3;
   - as 4 direções do direcional;
   - analógico esquerdo para a direita e para baixo;
   - analógico direito para a direita e para baixo.
3. **Como cada etapa reconhece**:
   - **botão**: vira `bN`;
   - **direcional**: vira `hN.máscara`;
   - **eixo**: conta a partir de 60% de movimento, medido a partir do repouso de quando a etapa
     começou;
   - **gatilho**: se repousa no mínimo, vira eixo inteiro (`aN`); se repousa no meio, vira
     metade (`+aN`/`-aN`);
   - **analógico**: o sinal do movimento decide `~` (invertido);
   - **direcional analógico**: vira metade de eixo.

   Entre uma etapa e outra é preciso **soltar tudo**. Uma fonte que já está com outro controle
   é recusada, com aviso.
4. **Pronto: N controles ensinados** e o botão **Salvar**. A linha vai para
   `<dados>/controles/gamecontrollerdb.txt`, no formato do SDL, trocando a anterior do mesmo
   GUID. Ela vale na hora (`gamepad_remap`) e o app volta ao testador.

Conferido: registrar o DualShock 4 (sem driver) pelas 21 etapas gerou **exatamente** a linha
do SDL GameControllerDB (o touchpad não é perguntado), no simulador e no aparelho.

## Uso de recursos (aparelho)

| Medida | Valor |
|---|---|
| CPU do shell parado na aba Controles | 3,8% (outras telas: 3,1–3,2%) |
| RSS do shell com o app e um controle | 4,7 MB |
| Achar o mapa ao conectar | lê no máximo 1 + 8 bancos (~200 KB), uma vez por controle |
