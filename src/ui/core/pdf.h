/* pdf.h — extração do texto de um PDF para leitura refluível no Livros.
 *
 * Não renderiza páginas (isso exigiria MuPDF/Poppler, vários MB): extrai o
 * texto na ordem do conteúdo, junta as linhas de um parágrafo e marca o
 * início de cada página. PDF só de imagem (escaneado) não tem texto. */
#ifndef BF_PDF_H
#define BF_PDF_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;             /* UTF-8 */
    size_t len;
    size_t *page_off;       /* início de cada página do PDF em text */
    int pages;
    char title[160], author[120];
} pdf_text_t;

bool pdf_extract(const char *path, pdf_text_t *out, char *err, size_t errn);
void pdf_free(pdf_text_t *p);
#endif
