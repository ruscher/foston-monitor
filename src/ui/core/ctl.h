/* ctl.h — controle do shell por FIFO, para testes no aparelho pelo SSH.
 * /run/bigfoston/ctl (só root, modo 600): uma linha por comando
 *   app web | app home | home | app apps (grade com todos) | app webapp N | page N (grade)
 *   web <url ou ação do scr_web_demo>
 * Sem custo quando ninguém escreve (um read não bloqueante a cada 300 ms). */
#ifndef BF_CTL_H
#define BF_CTL_H
void ctl_init(void);
#endif
