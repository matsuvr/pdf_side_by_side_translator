#ifndef PDF_READER_H
#define PDF_READER_H
#include <stdint.h>
#include <stddef.h>
typedef struct Reader Reader;
typedef struct ReaderSnapshot ReaderSnapshot;
typedef struct ReaderBytes { const uint8_t *data; size_t len; } ReaderBytes;
typedef struct ReaderView {
    uint64_t revision, image_id;
    uint32_t page, count, phase;
    ReaderBytes source, translation, message, png;
    double page_width, page_height; /* PDF points, independent of raster resolution. */
} ReaderView;
/* All calls originate on the UI thread. Paths and queries are borrowed UTF-8.
   Page numbers start at one. A snapshot owns its strings and PNG until freed.
   poll returns NULL if unchanged. PNG is omitted when image_id is already drawn. */
Reader *reader_new(void);
void reader_free(Reader *);
void reader_open(Reader *, const char *path, const char *password);
void reader_close(Reader *);
void reader_page(Reader *, uint32_t page);
void reader_find(Reader *, const char *query);
void reader_find_matching(Reader *, const char *query, uint32_t backwards, uint32_t match_case);
void reader_cancel_find(Reader *);
void reader_translation_options(Reader *, uint32_t automatic, uint32_t easy);
void reader_model(Reader *, const char *path);
ReaderSnapshot *reader_poll(Reader *, uint64_t after_revision, uint64_t drawn_image, ReaderView *out);
void reader_snapshot_free(ReaderSnapshot *);
enum ReaderPhase { ReaderIdle, ReaderLoading, ReaderReadingImage, ReaderTranslating,
                   ReaderReady, ReaderEmpty, ReaderError, ReaderPassword, ReaderPaused };
#endif
