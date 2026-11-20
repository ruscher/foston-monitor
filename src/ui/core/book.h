/* book.h — carregamento de livros (TXT e EPUB) como texto UTF-8 puro.
 *
 * TXT: UTF-8 ou, se não for UTF-8 válido, Latin-1/Windows-1252 convertido.
 * EPUB: ZIP (descompactado com o inflate do lodepng) -> container.xml ->
 * OPF -> capítulos na ordem do "spine" -> XHTML sem marcações, com parágrafos,
 * títulos e quebras de linha preservados e entidades decodificadas. Imagens,
 * tabelas e estilos ficam de fora. PDF: o texto extraído por pdf.c (sem
 * renderizar as páginas). SVG não vira texto: o Livros abre no visualizador. */
#ifndef BF_BOOK_H
#define BF_BOOK_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { BK_TXT, BK_EPUB, BK_PDF, BK_SVG, BK_NONE } book_kind_t;

typedef struct { size_t off; char title[96]; } book_chapter_t;

typedef struct {
    char *text;          /* UTF-8, terminado em NUL */
    size_t len;
    char title[160];     /* do OPF/Info do PDF, ou do nome do arquivo */
    char author[120];
    book_kind_t kind;
    book_chapter_t *chapters;   /* capítulos (EPUB) ou páginas (PDF) */
    int n_chapters;
} book_t;

/* err recebe uma explicação curta em caso de falha */
bool book_load(const char *path, book_t *b, char *err, size_t errn);
void book_free(book_t *b);
bool book_is_supported(const char *name);
book_kind_t book_kind(const char *name);
/* capa JPEG de um EPUB (malloc), ou NULL */
unsigned char *book_cover_jpeg(const char *path, size_t *len);
/* índice do capítulo que contém `off` (-1 se não há capítulos) */
int  book_chapter_at(const book_t *b, size_t off);
#endif
