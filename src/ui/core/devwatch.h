/* devwatch.h — avisos na tela quando um dispositivo é conectado ou removido
 * (fone, carregador, PC pela USB, cartão SD). Só no aparelho; no simulador as
 * fontes não existem e nada é anunciado. */
#ifndef BF_DEVWATCH_H
#define BF_DEVWATCH_H

void devwatch_init(void);
/* modo --screenshot: mostra o aviso de um dispositivo (0 fone, 1 carregador,
 * 2 USB, 3 SD) conectado (on = 1) ou removido (0) */
void devwatch_demo(int dev, int on);
#endif
