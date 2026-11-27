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
} ReaderView;
/* All calls originate on the UI thread. Paths and queries are borrowed UTF-8.
   Page numbers start at one. A snapshot owns its strings and PNG until freed.
   poll returns NULL if unchanged. PNG is omitted when image_id is already drawn. */
Reader *reader_new(void);
void reader_free(Reader *);
void reader_open(Reader *, const char *path, const char *password);
void reader_page(Reader *, uint32_t page);
void reader_find(Reader *, const char *query);
ReaderSnapshot *reader_poll(Reader *, uint64_t after_revision, uint64_t drawn_image, ReaderView *out);
void reader_snapshot_free(ReaderSnapshot *);
enum ReaderPhase { ReaderIdle, ReaderLoading, ReaderReadingImage, ReaderTranslating,
                   ReaderReady, ReaderEmpty, ReaderError, ReaderPassword };
#endif
