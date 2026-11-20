/* book.h — carregamento de livros (TXT e EPUB) como texto UTF-8 puro.
 *
 * TXT: UTF-8 ou, se não for UTF-8 válido, Latin-1/Windows-1252 convertido.
 * EPUB: ZIP (descompactado com o inflate do lodepng) -> container.xml ->
 * OPF -> capítulos na ordem do "spine" -> XHTML sem marcações, com parágrafos,
 * títulos e quebras de linha preservados e entidades decodificadas. Imagens,
 * tabelas e estilos ficam de fora. */
#ifndef BF_BOOK_H
#define BF_BOOK_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *text;          /* UTF-8, terminado em NUL */
    size_t len;
    char title[160];     /* do OPF (EPUB) ou do nome do arquivo */
    char author[120];
} book_t;

/* err recebe uma explicação curta em caso de falha */
bool book_load(const char *path, book_t *b, char *err, size_t errn);
void book_free(book_t *b);
bool book_is_supported(const char *name);
#endif
