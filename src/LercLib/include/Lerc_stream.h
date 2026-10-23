#ifndef LERC_STREAM_H
#define LERC_STREAM_H

#include "Lerc_c_api.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-only Lerc2 validation. This is a separate API from lerc_decode.
 * The blob decoder remains the sample oracle. The parser pulls bytes and
 * does not seek. One unread byte of lookahead is internal.
 *
 * A zero status means the report was filled. Findings may still be present.
 * Nonzero means the call itself was refused (null reader, or a sample
 * buffer missing when discard is off).
 *
 * Payload parsed in this slice: empty masks, stored RLE masks, constant
 * bands, per-depth constant ranges, raw one-sweep samples, tiled
 * bit-stuffing, byte or char Huffman, and lossless float or double Huffman.
 * An unimplemented mode stays category unsupported, not corrupt.
 *
 * When mask is non-NULL, each published band appends one byte per pixel,
 * 1 for valid and 0 for invalid. A NULL mask is still parsed.
 */

#define LERC_STREAM_DISCARD 1u

#define LERC_STREAM_OFFSET_EXACT 1u
#define LERC_STREAM_LENGTH_PRESENT 2u

#define LERC_STREAM_CAT_FORMAT 1u
#define LERC_STREAM_CAT_CODEC 2u
#define LERC_STREAM_CAT_LIMIT 3u
#define LERC_STREAM_CAT_IO 4u
#define LERC_STREAM_CAT_UNSUPPORTED 5u

#define LERC_STREAM_CODE_FILE_KEY 1u
#define LERC_STREAM_CODE_TRUNCATED 2u
#define LERC_STREAM_CODE_VERSION 3u
#define LERC_STREAM_CODE_CHECKSUM 4u
#define LERC_STREAM_CODE_HEADER_FIELD 5u
#define LERC_STREAM_CODE_BLOB_EXTENT 6u
#define LERC_STREAM_CODE_MASK 7u
#define LERC_STREAM_CODE_CONSUMPTION 8u
#define LERC_STREAM_CODE_UNSUPPORTED 9u
#define LERC_STREAM_CODE_TRAILING 10u
#define LERC_STREAM_CODE_READER 11u
#define LERC_STREAM_CODE_BUFFER 12u

typedef struct lerc_stream_finding {
	uint64_t offset;
	uint64_t length;
	uint32_t code;
	uint32_t category;
	uint32_t flags;
} lerc_stream_finding;

/* Pull up to n bytes. Return 0 and set *n_out to the number stored.
 * A short read, including 0, means no further byte is available.
 * Return nonzero on a reader fault. Do not deliver more than n.
 */
typedef struct lerc_stream_reader {
	int (*read)(void *ctx, unsigned char *dst, size_t n, size_t *n_out);
	void *ctx;
} lerc_stream_reader;

typedef struct lerc_stream_report {
	uint32_t options;
	unsigned char *samples;
	size_t sample_capacity;
	size_t sample_bytes;
	lerc_stream_finding *findings;
	uint32_t findings_capacity;
	uint32_t findings_count;
	uint32_t findings_omitted;
	uint64_t bytes_consumed;
	uint32_t stopped;
	unsigned char *mask;
	size_t mask_capacity;
	size_t mask_bytes;
} lerc_stream_report;

/* Walk the reader and fill the out-fields of report. */
LERCDLL_API lerc_status lerc_stream_parse(const lerc_stream_reader *reader, lerc_stream_report *report);

typedef struct lerc_stream_memory {
	const unsigned char *data;
	size_t size;
	size_t pos;
} lerc_stream_memory;

/* Memory pull. Fills as many bytes as remain, which may be the whole request. */
LERCDLL_API int lerc_stream_memory_read(void *ctx, unsigned char *dst, size_t n, size_t *n_out);

#ifdef __cplusplus
}
#endif

#endif
