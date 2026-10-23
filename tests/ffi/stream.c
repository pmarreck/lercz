/* Oracle: lerc_encode / lerc_decode. The stream parser is a separate API. */
#include "Lerc_c_api.h"
#include "Lerc_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *msg)
{
	fprintf(stderr, "stream: %s\n", msg);
	g_failures++;
}

typedef struct {
	const unsigned char *data;
	size_t size;
	size_t pos;
	size_t max_chunk;
} Chunk;

static int chunk_read(void *ctx, unsigned char *dst, size_t n, size_t *n_out)
{
	Chunk *c = ctx;
	size_t avail = c->pos < c->size ? c->size - c->pos : 0;
	size_t take = n < avail ? n : avail;
	if (c->max_chunk && take > c->max_chunk)
		take = c->max_chunk;
	if (take)
		memcpy(dst, c->data + c->pos, take);
	c->pos += take;
	*n_out = take;
	return 0;
}

static int encode_blob(const void *data, int version, unsigned dt, int depth, int cols, int rows,
	int bands, unsigned char *blob, unsigned cap, unsigned *n)
{
	if (version < 0) {
		if (lerc_computeCompressedSize(data, dt, depth, cols, rows, bands, 0, NULL, 0, n))
			return 1;
		return lerc_encode(data, dt, depth, cols, rows, bands, 0, NULL, 0, blob, cap, n) != 0;
	}
	if (lerc_computeCompressedSizeForVersion(data, version, dt, depth, cols, rows, bands, 0, NULL, 0, n))
		return 1;
	return lerc_encodeForVersion(data, version, dt, depth, cols, rows, bands, 0, NULL, 0, blob, cap, n) != 0;
}

static int parse(const unsigned char *blob, unsigned n, size_t chunk, uint32_t options,
	unsigned char *samples, size_t cap, lerc_stream_finding *findings, uint32_t slots,
	lerc_stream_report *out)
{
	Chunk mem = {blob, n, 0, chunk};
	lerc_stream_reader reader = {chunk_read, &mem};
	memset(out, 0, sizeof(*out));
	out->options = options;
	out->samples = samples;
	out->sample_capacity = cap;
	out->findings = findings;
	out->findings_capacity = slots;
	return lerc_stream_parse(&reader, out) != 0;
}

static int encode_masked(const void *data, unsigned dt, int depth, int cols, int rows, int bands,
	int nmasks, const unsigned char *mask, unsigned char *blob, unsigned cap, unsigned *n)
{
	if (lerc_computeCompressedSize(data, dt, depth, cols, rows, bands, nmasks, mask, 0, n))
		return 1;
	return lerc_encode(data, dt, depth, cols, rows, bands, nmasks, mask, 0, blob, cap, n) != 0;
}

static int parse_masked(const unsigned char *blob, unsigned n, size_t chunk, uint32_t options,
	unsigned char *samples, size_t cap, unsigned char *mask, size_t mask_cap,
	lerc_stream_finding *findings, uint32_t slots, lerc_stream_report *out)
{
	Chunk mem = {blob, n, 0, chunk};
	lerc_stream_reader reader = {chunk_read, &mem};
	memset(out, 0, sizeof(*out));
	out->options = options;
	out->samples = samples;
	out->sample_capacity = cap;
	out->mask = mask;
	out->mask_capacity = mask_cap;
	out->findings = findings;
	out->findings_capacity = slots;
	return lerc_stream_parse(&reader, out) != 0;
}

static void expect_masked(const char *name, const unsigned char *blob, unsigned n, size_t chunk,
	const void *oracle, size_t nbytes, const unsigned char *mask, size_t npix)
{
	unsigned char got[1024];
	unsigned char gmask[128];
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	memset(got, 0xA5, sizeof(got));
	memset(gmask, 0xA5, sizeof(gmask));
	if (parse_masked(blob, n, chunk, 0, got, sizeof(got), gmask, sizeof(gmask), findings, 8, &report)) {
		fail(name);
		return;
	}
	if (report.findings_count || report.findings_omitted || report.stopped || report.bytes_consumed != n ||
		report.sample_bytes != nbytes || report.mask_bytes != npix ||
		memcmp(got, oracle, nbytes) != 0 || memcmp(gmask, mask, npix) != 0) {
		fprintf(stderr, "stream: %s mask count=%u stopped=%u consumed=%llu samples=%zu mask_bytes=%zu\n",
			name, report.findings_count, report.stopped, (unsigned long long)report.bytes_consumed,
			report.sample_bytes, report.mask_bytes);
		fail(name);
	}
}

static void expect_clean(const char *name, const unsigned char *blob, unsigned n, size_t chunk,
	const void *oracle, size_t nbytes)
{
	unsigned char got[512];
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	memset(got, 0xA5, sizeof(got));
	if (parse(blob, n, chunk, 0, got, sizeof(got), findings, 8, &report)) {
		fail(name);
		return;
	}
	if (report.findings_count || report.findings_omitted || report.stopped || report.bytes_consumed != n ||
		report.sample_bytes != nbytes || memcmp(got, oracle, nbytes) != 0) {
		fprintf(stderr, "stream: %s clean count=%u omitted=%u stopped=%u consumed=%llu samples=%zu\n",
			name, report.findings_count, report.findings_omitted, report.stopped,
			(unsigned long long)report.bytes_consumed, report.sample_bytes);
		fail(name);
	}
}

static void expect_exact(const char *name, const lerc_stream_report *report, const lerc_stream_finding *want,
	uint32_t nwant, uint32_t stopped, uint64_t consumed)
{
	uint32_t i;
	if (report->findings_count != nwant || report->findings_omitted || report->stopped != stopped ||
		report->bytes_consumed != consumed) {
		fprintf(stderr, "stream: %s count=%u omitted=%u stopped=%u consumed=%llu\n", name,
			report->findings_count, report->findings_omitted, report->stopped,
			(unsigned long long)report->bytes_consumed);
		fail(name);
		return;
	}
	for (i = 0; i < nwant; i++) {
		const lerc_stream_finding *g = &report->findings[i];
		if (g->offset != want[i].offset || g->code != want[i].code || g->category != want[i].category ||
			(g->flags & LERC_STREAM_OFFSET_EXACT) == 0) {
			fprintf(stderr, "stream: %s finding %u offset=%llu code=%u cat=%u flags=%u\n", name, i,
				(unsigned long long)g->offset, g->code, g->category, g->flags);
			fail(name);
			return;
		}
		if (want[i].flags & LERC_STREAM_LENGTH_PRESENT) {
			if ((g->flags & LERC_STREAM_LENGTH_PRESENT) == 0 || g->length != want[i].length) {
				fprintf(stderr, "stream: %s finding %u length=%llu\n", name, i,
					(unsigned long long)g->length);
				fail(name);
				return;
			}
		}
	}
}

static void test_const_versions(void)
{
	const double data[4] = {3, 3, 3, 3};
	double oracle[4];
	unsigned char blob[1024];
	unsigned n = 0;
	int version;
	for (version = 2; version <= 6; version++) {
		char name[32];
		snprintf(name, sizeof(name), "const-v%d", version);
		if (encode_blob(data, version, 7, 1, 2, 2, 1, blob, sizeof(blob), &n) ||
			lerc_decode(blob, n, 0, NULL, 1, 2, 2, 1, 7, oracle)) {
			fail(name);
			continue;
		}
		expect_clean(name, blob, n, 0, oracle, sizeof(oracle));
		expect_clean(name, blob, n, 3, oracle, sizeof(oracle));
	}
}

static void test_one_sweep(void)
{
	/* int is outside Huffman. A 32-bit spread is larger tiled than raw. */
	const int one[4] = {0x12345678, -0x23456789, 0x3456789A, -0x456789AB};
	int oracle[4];
	unsigned char blob[2048];
	unsigned n = 0;
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	if (encode_blob(one, -1, 4, 1, 2, 2, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, 2, 2, 1, 4, oracle))
		fail("one-sweep encode");
	else {
		expect_clean("one-sweep", blob, n, 0, oracle, sizeof(oracle));
		expect_clean("one-sweep", blob, n, 3, oracle, sizeof(oracle));
		if (parse(blob, n, 3, LERC_STREAM_DISCARD, NULL, 0, findings, 8, &report) ||
			report.findings_count || report.bytes_consumed != n || report.sample_bytes)
			fail("one-sweep discard");
	}
}

static int tile_flag(const unsigned char *blob, unsigned n, int depth, int es, unsigned *at_out, unsigned *flag);

static void test_integer_valued_doubles(void)
{
	/* Integer-valued floats are rewritten to maxZError 0.5 and tiled.
	 * The byte after the zero flag is a tile comprFlag, not a Huffman mode.
	 */
	const double one[4] = {1, 2, 3, 4};
	double oracle[4];
	unsigned char blob[2048];
	unsigned char got[sizeof(oracle)];
	unsigned n = 0;
	unsigned at = 0, flag = 1;
	int i;
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	if (encode_blob(one, -1, 7, 1, 2, 2, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, 2, 2, 1, 7, oracle) ||
		tile_flag(blob, n, 1, (int)sizeof(double), &at, &flag) || flag != 0) {
		fprintf(stderr, "stream: int-double fixture n=%u flag=%u\n", n, flag);
		fail("int-double fixture");
		return;
	}
	memset(got, 0xA5, sizeof(got));
	if (parse(blob, n, 3, 0, got, sizeof(got), findings, 8, &report) ||
		report.findings_count || report.findings_omitted || report.stopped ||
		report.bytes_consumed != n || report.sample_bytes != sizeof(oracle) ||
		memcmp(got, oracle, sizeof(oracle)) != 0) {
		fprintf(stderr, "stream: int-double count=%u consumed=%llu samples=%zu code=%u\n",
			report.findings_count, (unsigned long long)report.bytes_consumed,
			report.sample_bytes, report.findings_count ? findings[0].code : 0);
		for (i = 0; i < 4; i++) {
			double g = 0;
			memcpy(&g, got + (size_t)i * sizeof(double), sizeof(g));
			fprintf(stderr, "stream: int-double [%d] oracle=%a got=%a\n", i, oracle[i], g);
		}
		fail("int-double");
	}
}

/* Huffman-eligible bytes whose encoder still picked tiling (mode byte 0). */
static void test_byte_tiles_mode0(void)
{
	enum { N = 8 };
	unsigned char data[N * N];
	unsigned char oracle[N * N];
	unsigned char blob[2048];
	unsigned n = 0, at = 0, flag = 1;
	int i;
	for (i = 0; i < N * N; i++)
		data[i] = (unsigned char)(i % N);
	if (encode_blob(data, -1, 1, 1, N, N, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, N, N, 1, 1, oracle) ||
		tile_flag(blob, n, 1, 1, &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != 0) {
		fprintf(stderr, "stream: byte-tile fixture n=%u flag=%u mode=%u\n", n, flag,
			at + 1 < n ? blob[at + 1] : 0);
		fail("byte-tile fixture");
		return;
	}
	expect_clean("byte-tile", blob, n, 3, oracle, sizeof(oracle));
}

static void expect_huffman_match(const char *name, const void *data, unsigned dt, int es,
	int cols, int rows, unsigned mode)
{
	unsigned char blob[8192];
	unsigned char oracle[512];
	unsigned n = 0, at = 0, flag = 1;
	size_t nbytes = (size_t)cols * (size_t)rows * (size_t)es;
	if (nbytes > sizeof(oracle) ||
		encode_blob(data, -1, dt, 1, cols, rows, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, cols, rows, 1, dt, oracle) ||
		tile_flag(blob, n, 1, es, &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != mode) {
		fprintf(stderr, "stream: %s fixture n=%u flag=%u mode=%u want=%u\n", name, n, flag,
			at + 1 < n ? blob[at + 1] : 0, mode);
		fail(name);
		return;
	}
	expect_clean(name, blob, n, 3, oracle, nbytes);
}

static void expect_sized(const char *name, const unsigned char *blob, unsigned n, size_t chunk,
	const void *oracle, size_t nbytes)
{
	unsigned char *got = malloc(nbytes ? nbytes : 1);
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	if (!got) {
		fail(name);
		return;
	}
	memset(got, 0xA5, nbytes ? nbytes : 1);
	if (parse(blob, n, chunk, 0, got, nbytes, findings, 8, &report) ||
		report.findings_count || report.findings_omitted || report.stopped ||
		report.bytes_consumed != n || report.sample_bytes != nbytes ||
		memcmp(got, oracle, nbytes) != 0) {
		fprintf(stderr, "stream: %s count=%u omitted=%u stopped=%u consumed=%llu samples=%zu code=%u off=%llu\n",
			name, report.findings_count, report.findings_omitted, report.stopped,
			(unsigned long long)report.bytes_consumed, report.sample_bytes,
			report.findings_count ? findings[0].code : 0,
			report.findings_count ? (unsigned long long)findings[0].offset : 0);
		fail(name);
	}
	free(got);
}

/* Lossless float and double mode 3 must match the blob decoder. */
static void expect_float_huff(const char *name, const void *data, unsigned dt, int es, int depth,
	int cols, int rows)
{
	size_t nbytes = (size_t)cols * (size_t)rows * (size_t)depth * (size_t)es;
	unsigned char *blob = malloc(nbytes + 65536);
	void *oracle = malloc(nbytes ? nbytes : 1);
	unsigned n = 0, at = 0, flag = 1;
	if (!blob || !oracle) {
		free(blob);
		free(oracle);
		fail(name);
		return;
	}
	if (encode_blob(data, -1, dt, depth, cols, rows, 1, blob, (unsigned)(nbytes + 65536), &n) ||
		lerc_decode(blob, n, 0, NULL, depth, cols, rows, 1, dt, oracle) ||
		tile_flag(blob, n, depth, es, &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != 3) {
		fprintf(stderr, "stream: %s fixture n=%u flag=%u mode=%u\n", name, n, flag,
			at + 1 < n ? blob[at + 1] : 0);
		fail(name);
	} else
		expect_sized(name, blob, n, 3, oracle, nbytes);
	free(blob);
	free(oracle);
}

/* Byte and char Huffman must match the blob decoder. */
static void test_huffman_unsupported(void)
{
	enum { R = 8, T = 16 };
	unsigned char ramp[R * R];
	unsigned char text[T * T];
	static const char phrase[] = "LERC huffman wins on smooth bytes.";
	int i;
	for (i = 0; i < R * R; i++)
		ramp[i] = (unsigned char)(i & 255);
	for (i = 0; i < T * T; i++)
		text[i] = (unsigned char)phrase[i % (int)sizeof(phrase)];
	expect_huffman_match("huff-delta", ramp, 1, 1, R, R, 1);
	expect_huffman_match("huff-plain", text, 1, 1, T, T, 2);
	{
		unsigned char blob[2048];
		unsigned char samples[16];
		unsigned n = 0;
		lerc_stream_finding findings[4];
		lerc_stream_report report;
		if (encode_blob(ramp, -1, 1, 1, R, R, 1, blob, sizeof(blob), &n))
			fail("huff-discard encode");
		else {
			memset(samples, 0xA5, sizeof(samples));
			if (parse(blob, n, 3, LERC_STREAM_DISCARD, samples, sizeof(samples), findings, 4, &report) ||
				report.findings_count || report.bytes_consumed != n || report.sample_bytes || samples[0] != 0xA5)
				fail("huff-discard");
		}
	}
	{
		signed char cold[T * T];
		for (i = 0; i < T * T; i++)
			cold[i] = (signed char)((i % 40) - 20);
		expect_huffman_match("huff-char", cold, 0, 1, T, T, 1);
	}
	{
		enum { M = 8 };
		unsigned char deep[M * M * 2], oracle[M * M * 2], blob[4096];
		unsigned n = 0, at = 0, flag = 1;
		for (i = 0; i < M * M; i++) {
			deep[i * 2] = (unsigned char)(i & 255);
			deep[i * 2 + 1] = (unsigned char)((i * 3) & 255);
		}
		if (encode_blob(deep, -1, 1, 2, M, M, 1, blob, sizeof(blob), &n) ||
			lerc_decode(blob, n, 0, NULL, 2, M, M, 1, 1, oracle) ||
			tile_flag(blob, n, 2, 1, &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != 1) {
			fprintf(stderr, "stream: huff-depth fixture n=%u flag=%u mode=%u\n", n, flag,
				at + 1 < n ? blob[at + 1] : 0);
			fail("huff-depth fixture");
		} else
			expect_clean("huff-depth", blob, n, 3, oracle, sizeof(oracle));
	}
	{
		enum { N = 16 };
		unsigned char data[N * N], mask[N * N], oracle[N * N], omask[N * N];
		unsigned char got[N * N], gmask[N * N], blob[4096];
		unsigned n = 0, at = 0, flag = 1;
		lerc_stream_finding findings[8];
		lerc_stream_report report;
		for (i = 0; i < N * N; i++) {
			data[i] = (unsigned char)(i & 255);
			mask[i] = (unsigned char)(i < 40 || (i % 5) == 0);
		}
		if (encode_masked(data, 1, 1, N, N, 1, 1, mask, blob, sizeof(blob), &n) ||
			lerc_decode(blob, n, 1, omask, 1, N, N, 1, 1, oracle) ||
			tile_flag(blob, n, 1, 1, &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != 1) {
			fprintf(stderr, "stream: huff-mask fixture n=%u flag=%u mode=%u\n", n, flag,
				at + 1 < n ? blob[at + 1] : 0);
			fail("huff-mask fixture");
		} else {
			memset(got, 0xA5, sizeof(got));
			memset(gmask, 0xA5, sizeof(gmask));
			if (parse_masked(blob, n, 3, 0, got, sizeof(got), gmask, sizeof(gmask), findings, 8, &report) ||
				report.findings_count || report.bytes_consumed != n ||
				report.sample_bytes != sizeof(oracle) || report.mask_bytes != N * N ||
				memcmp(got, oracle, sizeof(oracle)) != 0 || memcmp(gmask, omask, sizeof(omask)) != 0) {
				fprintf(stderr, "stream: huff-mask count=%u consumed=%llu samples=%zu mask=%zu code=%u off=%llu\n",
					report.findings_count, (unsigned long long)report.bytes_consumed,
					report.sample_bytes, report.mask_bytes,
					report.findings_count ? findings[0].code : 0,
					report.findings_count ? (unsigned long long)findings[0].offset : 0);
				fail("huff-mask");
			}
		}
	}
}

static void test_discard_and_bands(void)
{
	const double two[8] = {1, 1, 1, 1, 9, 9, 9, 9};
	const double depth[8] = {1, 5, 1, 5, 1, 5, 1, 5};
	double oracle[8];
	unsigned char blob[2048];
	unsigned n = 0;
	if (encode_blob(two, -1, 7, 1, 2, 2, 2, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, 2, 2, 2, 7, oracle))
		fail("bands encode");
	else
		expect_clean("two-band", blob, n, 1, oracle, sizeof(oracle));
	if (encode_blob(depth, -1, 7, 2, 2, 2, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 2, 2, 2, 1, 7, oracle))
		fail("depth encode");
	else
		expect_clean("per-depth-const", blob, n, 5, oracle, sizeof(oracle));
}

static void test_truncation_checksum_fields(void)
{
	const double data[4] = {3, 3, 3, 3};
	unsigned char blob[1024];
	unsigned char samples[32];
	unsigned n = 0;
	lerc_stream_finding findings[8];
	lerc_stream_finding want[4];
	lerc_stream_report report;
	if (encode_blob(data, 6, 7, 1, 2, 2, 1, blob, sizeof(blob), &n) || n < 50 || blob[6] != 6) {
		fail("fixture");
		return;
	}
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n - 1, 3, 0, samples, sizeof(samples), findings, 8, &report))
		fail("trunc call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = n - 1;
		want[0].length = 1;
		want[0].code = LERC_STREAM_CODE_TRUNCATED;
		want[0].category = LERC_STREAM_CAT_FORMAT;
		want[0].flags = LERC_STREAM_LENGTH_PRESENT;
		expect_exact("truncated", &report, want, 1, 1, n - 1);
		if (report.sample_bytes || samples[0] != 0xA5)
			fail("truncated samples");
	}
	blob[48] ^= 1;
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 0, 0, samples, sizeof(samples), findings, 8, &report))
		fail("checksum call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = 10;
		want[0].code = LERC_STREAM_CODE_CHECKSUM;
		want[0].category = LERC_STREAM_CAT_CODEC;
		expect_exact("checksum", &report, want, 1, 0, n);
		if (report.sample_bytes || samples[0] != 0xA5)
			fail("checksum samples");
	}
	blob[48] ^= 1;
	blob[14] = 0;
	blob[15] = 0;
	blob[16] = 0;
	blob[17] = 0;
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 4, 0, samples, sizeof(samples), findings, 8, &report))
		fail("fields call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = 14;
		want[0].code = LERC_STREAM_CODE_HEADER_FIELD;
		want[0].category = LERC_STREAM_CAT_FORMAT;
		want[1].offset = 26;
		want[1].code = LERC_STREAM_CODE_HEADER_FIELD;
		want[1].category = LERC_STREAM_CAT_FORMAT;
		want[2].offset = 10;
		want[2].code = LERC_STREAM_CODE_CHECKSUM;
		want[2].category = LERC_STREAM_CAT_CODEC;
		expect_exact("nRows", &report, want, 3, 0, n);
		if (samples[0] != 0xA5)
			fail("nRows samples");
	}
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 0, 0, samples, sizeof(samples), findings, 1, &report))
		fail("omit call");
	else if (report.findings_count != 1 || report.findings_omitted != 2 || findings[0].offset != 14)
		fail("omit");
}

static void test_key_version_trailing(void)
{
	const double data[4] = {3, 3, 3, 3};
	unsigned char blob[1024];
	unsigned char samples[32];
	unsigned n = 0;
	lerc_stream_finding findings[4];
	lerc_stream_finding want[2];
	lerc_stream_report report;
	if (encode_blob(data, 6, 7, 1, 2, 2, 1, blob, sizeof(blob), &n)) {
		fail("key fixture");
		return;
	}
	blob[0] ^= 0xFF;
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 2, 0, samples, sizeof(samples), findings, 4, &report))
		fail("key call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = 0;
		want[0].code = LERC_STREAM_CODE_FILE_KEY;
		want[0].category = LERC_STREAM_CAT_FORMAT;
		expect_exact("file key", &report, want, 1, 1, 1);
	}
	blob[0] ^= 0xFF;
	blob[6] = 99;
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 0, 0, samples, sizeof(samples), findings, 4, &report))
		fail("version call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = 6;
		want[0].code = LERC_STREAM_CODE_VERSION;
		want[0].category = LERC_STREAM_CAT_FORMAT;
		expect_exact("version", &report, want, 1, 1, 10);
	}
	blob[6] = 6;
	blob[n] = 0xFF;
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n + 1, 3, 0, samples, sizeof(samples), findings, 4, &report))
		fail("trailing call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = n;
		want[0].length = 1;
		want[0].code = LERC_STREAM_CODE_TRAILING;
		want[0].category = LERC_STREAM_CAT_FORMAT;
		want[0].flags = LERC_STREAM_LENGTH_PRESENT;
		expect_exact("trailing", &report, want, 1, 0, n + 1);
		if (report.sample_bytes != 4 * sizeof(double))
			fail("trailing samples");
	}
}

static void test_version_mutation(void)
{
	const double data[] = {1, -9999, 3, 4, 5, 6, -9999, 8};
	const unsigned char uses[] = {1, 1};
	const double nodata[] = {-9999, -9999};
	unsigned char blob[4096];
	unsigned char samples[128];
	unsigned n = 0;
	double decoded[8], decoded_nodata[2];
	unsigned char decoded_uses[2], mask[4];
	lerc_stream_finding findings[16];
	lerc_stream_report report;
	uint32_t i;
	if (lerc_encode_4D(data, 7, 2, 2, 1, 2, 0, NULL, 0, blob, sizeof(blob), &n, uses, nodata) || blob[6] != 6) {
		fail("mutation fixture");
		return;
	}
	blob[6] ^= 2;
	if (!lerc_decode_4D(blob, n, 2, mask, 2, 2, 1, 2, 7, decoded, decoded_uses, decoded_nodata))
		fail("oracle still accepts version mutation");
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 3, 0, samples, sizeof(samples), findings, 16, &report)) {
		fail("mutation call");
		return;
	}
	if (!report.findings_count || report.sample_bytes || samples[0] != 0xA5)
		fail("mutation accepted");
	for (i = 0; i < report.findings_count; i++) {
		if ((findings[i].flags & LERC_STREAM_OFFSET_EXACT) == 0 || findings[i].offset > n)
			fail("mutation offset");
	}
}

static void test_stored_mask(void)
{
	const double grid[16] = {
		3, 3, 3, 3, 3, 3, 3, 3,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	const unsigned char grid_mask[16] = {
		1, 1, 1, 1, 1, 1, 1, 1,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	const int sweep[4] = {0x12345678, 0, 0, -0x23456789};
	const unsigned char sweep_mask[4] = {1, 0, 0, 1};
	double d_oracle[16];
	int i_oracle[4];
	unsigned char m_oracle[16];
	unsigned char blob[2048];
	unsigned char kept[128];
	unsigned n = 0;
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	if (encode_masked(grid, 7, 1, 8, 2, 1, 1, grid_mask, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 1, m_oracle, 1, 8, 2, 1, 7, d_oracle))
		fail("mask const encode");
	else {
		unsigned char samples[256];
		expect_masked("mask-const", blob, n, 1, d_oracle, sizeof(d_oracle), m_oracle, 16);
		expect_masked("mask-const", blob, n, 5, d_oracle, sizeof(d_oracle), m_oracle, 16);
		memset(samples, 0xA5, sizeof(samples));
		memset(kept, 0xA5, sizeof(kept));
		if (parse_masked(blob, n, 0, 0, samples, sizeof(samples), kept, 1, findings, 8, &report) ||
			report.findings_count != 1 || findings[0].code != LERC_STREAM_CODE_BUFFER ||
			findings[0].category != LERC_STREAM_CAT_LIMIT || report.sample_bytes || report.mask_bytes ||
			samples[0] != 0xA5 || kept[0] != 0xA5)
			fail("mask cap");
		memset(kept, 0xA5, sizeof(kept));
		if (parse_masked(blob, n, 3, LERC_STREAM_DISCARD, NULL, 0, kept, sizeof(kept), findings, 8, &report) ||
			report.findings_count || report.bytes_consumed != n || report.sample_bytes || report.mask_bytes ||
			kept[0] != 0xA5)
			fail("mask discard");
	}
	{
		double run_data[64];
		unsigned char run_mask[64];
		double run_oracle[64];
		unsigned char run_m[64];
		int pix;
		for (pix = 0; pix < 64; pix++) {
			run_mask[pix] = pix < 8 ? 1 : 0;
			run_data[pix] = run_mask[pix] ? 4.0 : 0.0;
		}
		if (encode_masked(run_data, 7, 1, 8, 8, 1, 1, run_mask, blob, sizeof(blob), &n) ||
			lerc_decode(blob, n, 1, run_m, 1, 8, 8, 1, 7, run_oracle))
			fail("mask run encode");
		else
			expect_masked("mask-run", blob, n, 4, run_oracle, sizeof(run_oracle), run_m, 64);
	}
	{
		const double bands[8] = {3, 0, 3, 0, 9, 0, 9, 0};
		const unsigned char band_mask[4] = {1, 0, 1, 0};
		double band_oracle[8];
		unsigned char band_m[8];
		if (encode_masked(bands, 7, 1, 2, 2, 2, 1, band_mask, blob, sizeof(blob), &n) ||
			lerc_decode(blob, n, 2, band_m, 1, 2, 2, 2, 7, band_oracle))
			fail("mask bands encode");
		else
			expect_masked("mask-bands", blob, n, 2, band_oracle, sizeof(band_oracle), band_m, 8);
	}
	if (encode_masked(sweep, 4, 1, 2, 2, 1, 1, sweep_mask, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 1, m_oracle, 1, 2, 2, 1, 4, i_oracle))
		fail("mask sweep encode");
	else
		expect_masked("mask-sweep", blob, n, 3, i_oracle, sizeof(i_oracle), m_oracle, 4);
}

/* v6: 90-byte header, mask count, mask body, then per-depth min/max. */
static int tile_flag(const unsigned char *blob, unsigned n, int depth, int es, unsigned *at_out, unsigned *flag)
{
	int mask_n;
	unsigned at;
	if (n < 98 || blob[6] != 6)
		return -1;
	memcpy(&mask_n, blob + 90, 4);
	if (mask_n < 0)
		return -1;
	at = 94u + (unsigned)mask_n + (unsigned)depth * (unsigned)es * 2u;
	if (at >= n)
		return -1;
	*at_out = at;
	*flag = blob[at];
	return 0;
}

static void test_tiled_ints(void)
{
	enum { N = 16 };
	int data[N * N];
	int oracle[N * N];
	unsigned char blob[8192];
	unsigned char got[N * N * sizeof(int)];
	unsigned n = 0;
	int i;
	lerc_stream_finding findings[8];
	lerc_stream_report report;
	for (i = 0; i < N * N; i++)
		data[i] = (i * 17) & 3;
	if (encode_blob(data, -1, 4, 1, N, N, 1, blob, sizeof(blob), &n) ||
		lerc_decode(blob, n, 0, NULL, 1, N, N, 1, 4, oracle) || n <= 102 || blob[6] != 6) {
		fail("tile encode");
		return;
	}
	if (blob[102] != 0) {
		fprintf(stderr, "stream: tile fixture n=%u flag=%u\n", n, blob[102]);
		fail("tile fixture");
		return;
	}
	memset(got, 0xA5, sizeof(got));
	if (parse(blob, n, 3, 0, got, sizeof(got), findings, 8, &report) ||
		report.findings_count || report.findings_omitted || report.stopped ||
		report.bytes_consumed != n || report.sample_bytes != sizeof(oracle) ||
		memcmp(got, oracle, sizeof(oracle)) != 0) {
		fprintf(stderr, "stream: tiled count=%u stopped=%u consumed=%llu samples=%zu code=%u off=%llu\n",
			report.findings_count, report.stopped, (unsigned long long)report.bytes_consumed,
			report.sample_bytes, report.findings_count ? findings[0].code : 0,
			report.findings_count ? (unsigned long long)findings[0].offset : 0);
		fail("tiled");
	}
	memset(got, 0xA5, sizeof(got));
	if (parse(blob, n, 5, LERC_STREAM_DISCARD, got, sizeof(got), findings, 8, &report) ||
		report.findings_count || report.bytes_consumed != n || report.sample_bytes || got[0] != 0xA5)
		fail("tile discard");
	{
		enum { M = 8 };
		int masked[M * M], m_oracle[M * M];
		unsigned char mask[M * M], gmask[M * M], mgot[M * M];
		unsigned char mblob[4096];
		int mout[M * M];
		unsigned mn = 0, at = 0, flag = 1;
		for (i = 0; i < M * M; i++) {
			mask[i] = (unsigned char)(i < 20 || (i % 5) == 0);
			masked[i] = mask[i] ? (i * 3) & 7 : 0;
		}
		if (encode_masked(masked, 4, 1, M, M, 1, 1, mask, mblob, sizeof(mblob), &mn) ||
			lerc_decode(mblob, mn, 1, mgot, 1, M, M, 1, 4, m_oracle) ||
			tile_flag(mblob, mn, 1, 4, &at, &flag) || flag != 0) {
			fprintf(stderr, "stream: tile mask fixture n=%u flag=%u\n", mn, flag);
			fail("tile mask fixture");
		} else {
			memset(mout, 0xA5, sizeof(mout));
			memset(gmask, 0xA5, sizeof(gmask));
			if (parse_masked(mblob, mn, 3, 0, (unsigned char *)mout, sizeof(mout), gmask, sizeof(gmask),
				findings, 8, &report) || report.findings_count || report.bytes_consumed != mn ||
				report.sample_bytes != sizeof(m_oracle) || report.mask_bytes != M * M ||
				memcmp(mout, m_oracle, sizeof(m_oracle)) != 0 || memcmp(gmask, mgot, M * M) != 0) {
				fprintf(stderr, "stream: tile-mask count=%u consumed=%llu samples=%zu mask=%zu code=%u\n",
					report.findings_count, (unsigned long long)report.bytes_consumed,
					report.sample_bytes, report.mask_bytes,
					report.findings_count ? findings[0].code : 0);
				fail("tile mask");
			}
		}
	}
	{
		enum { M = 8 };
		int sparse[M * M], s_oracle[M * M];
		unsigned char sblob[4096];
		int sout[M * M];
		unsigned sn = 0, at = 0, flag = 1;
		for (i = 0; i < M * M; i++)
			sparse[i] = (i % 11) == 0 ? 1000 : 0;
		if (encode_blob(sparse, -1, 4, 1, M, M, 1, sblob, sizeof(sblob), &sn) ||
			lerc_decode(sblob, sn, 0, NULL, 1, M, M, 1, 4, s_oracle) ||
			tile_flag(sblob, sn, 1, 4, &at, &flag) || flag != 0) {
			fprintf(stderr, "stream: tile lut fixture n=%u flag=%u\n", sn, flag);
			fail("tile lut fixture");
		} else {
			memset(sout, 0xA5, sizeof(sout));
			if (parse(sblob, sn, 4, 0, (unsigned char *)sout, sizeof(sout), findings, 8, &report) ||
				report.findings_count || report.bytes_consumed != sn ||
				report.sample_bytes != sizeof(s_oracle) || memcmp(sout, s_oracle, sizeof(s_oracle)) != 0) {
				fprintf(stderr, "stream: tile-lut count=%u consumed=%llu samples=%zu code=%u off=%llu\n",
					report.findings_count, (unsigned long long)report.bytes_consumed,
					report.sample_bytes, report.findings_count ? findings[0].code : 0,
					report.findings_count ? (unsigned long long)findings[0].offset : 0);
				fail("tile lut");
			}
		}
	}
	{
		enum { M = 8 };
		int deep[M * M * 2], d_oracle[M * M * 2];
		unsigned char dblob[8192];
		int dout[M * M * 2];
		unsigned dn = 0, at = 0, flag = 1;
		for (i = 0; i < M * M; i++) {
			deep[i * 2] = i & 3;
			deep[i * 2 + 1] = i & 3;
		}
		if (encode_blob(deep, -1, 4, 2, M, M, 1, dblob, sizeof(dblob), &dn) ||
			lerc_decode(dblob, dn, 0, NULL, 2, M, M, 1, 4, d_oracle) ||
			tile_flag(dblob, dn, 2, 4, &at, &flag) || flag != 0) {
			fprintf(stderr, "stream: tile depth fixture n=%u flag=%u\n", dn, flag);
			fail("tile depth fixture");
		} else {
			memset(dout, 0xA5, sizeof(dout));
			if (parse(dblob, dn, 3, 0, (unsigned char *)dout, sizeof(dout), findings, 8, &report) ||
				report.findings_count || report.bytes_consumed != dn ||
				report.sample_bytes != sizeof(d_oracle) || memcmp(dout, d_oracle, sizeof(d_oracle)) != 0) {
				fprintf(stderr, "stream: tile-depth count=%u consumed=%llu samples=%zu code=%u off=%llu\n",
					report.findings_count, (unsigned long long)report.bytes_consumed,
					report.sample_bytes, report.findings_count ? findings[0].code : 0,
					report.findings_count ? (unsigned long long)findings[0].offset : 0);
				fail("tile depth");
			}
		}
	}
	{
		enum { M = 8 };
		int deep[M * M * 2], d_oracle[M * M * 2];
		unsigned char dblob[8192];
		int dout[M * M * 2];
		unsigned dn = 0, at = 0, flag = 1;
		for (i = 0; i < M * M; i++) {
			deep[i * 2] = i & 3;
			deep[i * 2 + 1] = 20 + (i & 7);
		}
		if (encode_blob(deep, -1, 4, 2, M, M, 1, dblob, sizeof(dblob), &dn) ||
			lerc_decode(dblob, dn, 0, NULL, 2, M, M, 1, 4, d_oracle) ||
			tile_flag(dblob, dn, 2, 4, &at, &flag) || flag != 0) {
			fprintf(stderr, "stream: tile depth-range fixture n=%u flag=%u\n", dn, flag);
			fail("tile depth-range fixture");
		} else {
			memset(dout, 0xA5, sizeof(dout));
			if (parse(dblob, dn, 3, 0, (unsigned char *)dout, sizeof(dout), findings, 8, &report) ||
				report.findings_count || report.bytes_consumed != dn ||
				report.sample_bytes != sizeof(d_oracle) || memcmp(dout, d_oracle, sizeof(d_oracle)) != 0) {
				fprintf(stderr, "stream: tile-range count=%u consumed=%llu samples=%zu code=%u off=%llu\n",
					report.findings_count, (unsigned long long)report.bytes_consumed,
					report.sample_bytes, report.findings_count ? findings[0].code : 0,
					report.findings_count ? (unsigned long long)findings[0].offset : 0);
				fail("tile depth-range");
			}
		}
	}
}

static void test_float_huffman(void)
{
	enum { S = 16, W = 32, D = 48 };
	double ramp[S * S];
	float delta[S * S];
	float cross[W * W];
	double wide[D * D];
	int i;
	for (i = 0; i < S * S; i++)
		ramp[i] = 0.25 * (double)(i % S) + 0.125;
	expect_float_huff("huff-double-rows", ramp, 7, (int)sizeof(double), 1, S, S);
	for (i = 0; i < S * S; i++)
		delta[i] = 0.1f * (float)(i & 15) + 0.03f * (float)(i / S);
	expect_float_huff("huff-float-delta", delta, 6, (int)sizeof(float), 1, S, S);
	for (i = 0; i < W * W; i++)
		cross[i] = 0.1f * (float)(i & 15) + 0.03f * (float)(i / W);
	expect_float_huff("huff-float-cross", cross, 6, (int)sizeof(float), 1, W, W);
	for (i = 0; i < D * D; i++)
		wide[i] = 0.25 * (double)(i % D) + 0.125;
	expect_float_huff("huff-double-wide", wide, 7, (int)sizeof(double), 1, D, D);
	{
		enum { L = 96, P = 32 };
		float *level = malloc(sizeof(float) * L * L);
		float *fdeep = malloc(sizeof(float) * P * P * 2);
		double *ddeep = malloc(sizeof(double) * P * P * 2);
		if (!level || !fdeep || !ddeep) {
			free(level);
			free(fdeep);
			free(ddeep);
			fail("huff-float-extra alloc");
		} else {
			for (i = 0; i < L * L; i++)
				level[i] = 0.1f * (float)(i & 255) + 0.03f * (float)(i / 128) +
					0.001f * (float)((i * 17) & 63);
			expect_float_huff("huff-float-level", level, 6, (int)sizeof(float), 1, L, L);
			for (i = 0; i < P * P; i++) {
				fdeep[i * 2] = 0.1f * (float)(i & 15) + 0.03f * (float)(i / P);
				fdeep[i * 2 + 1] = 0.2f * (float)(i & 15) + 0.01f * (float)(i / P);
				ddeep[i * 2] = 0.25 * (double)(i % P) + 0.125;
				ddeep[i * 2 + 1] = 0.5 * (double)(i % P) + 0.25;
			}
			expect_float_huff("huff-float-depth", fdeep, 6, (int)sizeof(float), 2, P, P);
			expect_float_huff("huff-double-depth", ddeep, 7, (int)sizeof(double), 2, P, P);
			free(level);
			free(fdeep);
			free(ddeep);
		}
	}
	{
		unsigned char blob[16384];
		unsigned char samples[16];
		unsigned n = 0, at = 0, flag = 1;
		lerc_stream_finding findings[4];
		lerc_stream_report report;
		if (encode_blob(delta, -1, 6, 1, S, S, 1, blob, sizeof(blob), &n) ||
			tile_flag(blob, n, 1, (int)sizeof(float), &at, &flag) || flag != 0 || at + 1 >= n || blob[at + 1] != 3)
			fail("huff-float-discard fixture");
		else {
			memset(samples, 0xA5, sizeof(samples));
			if (parse(blob, n, 3, LERC_STREAM_DISCARD, samples, sizeof(samples), findings, 4, &report) ||
				report.findings_count || report.bytes_consumed != n || report.sample_bytes || samples[0] != 0xA5)
				fail("huff-float-discard");
		}
	}
}

static void test_wrong_param(void)
{
	lerc_stream_report report;
	lerc_stream_reader reader = {chunk_read, NULL};
	memset(&report, 0, sizeof(report));
	if (lerc_stream_parse(NULL, &report) == 0)
		fail("null reader");
	if (lerc_stream_parse(&reader, NULL) == 0)
		fail("null report");
	report.samples = NULL;
	if (lerc_stream_parse(&reader, &report) == 0)
		fail("null samples");
}

int main(void)
{
	test_wrong_param();
	test_const_versions();
	test_one_sweep();
	test_stored_mask();
	test_tiled_ints();
	test_integer_valued_doubles();
	test_byte_tiles_mode0();
	test_huffman_unsupported();
	test_float_huffman();
	test_discard_and_bands();
	test_truncation_checksum_fields();
	test_key_version_trailing();
	test_version_mutation();
	if (g_failures)
		fprintf(stderr, "stream: %d failure(s)\n", g_failures);
	return g_failures ? 1 : 0;
}
