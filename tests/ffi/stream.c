/* Oracle: lerc_encode / lerc_decode. The stream parser is a separate API. */
#include "Lerc_c_api.h"
#include "Lerc_stream.h"

#include <stdio.h>
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

static void test_smooth_double_unsupported(void)
{
	const double one[4] = {1, 2, 3, 4};
	unsigned char blob[2048];
	unsigned char samples[64];
	unsigned n = 0;
	/* v6 header, mask count, per-depth min/max, then the one-sweep flag. */
	const uint64_t flag_at = 90 + 4 + 16;
	lerc_stream_finding findings[8];
	lerc_stream_finding want[1];
	lerc_stream_report report;
	if (encode_blob(one, -1, 7, 1, 2, 2, 1, blob, sizeof(blob), &n) || n <= flag_at || blob[flag_at] != 0) {
		fprintf(stderr, "stream: smooth fixture n=%u byte=%u\n", n,
			n > flag_at ? blob[flag_at] : 0);
		fail("smooth fixture");
		return;
	}
	memset(samples, 0xA5, sizeof(samples));
	if (parse(blob, n, 3, 0, samples, sizeof(samples), findings, 8, &report))
		fail("smooth call");
	else {
		memset(want, 0, sizeof(want));
		want[0].offset = flag_at;
		want[0].length = 1;
		want[0].code = LERC_STREAM_CODE_UNSUPPORTED;
		want[0].category = LERC_STREAM_CAT_UNSUPPORTED;
		want[0].flags = LERC_STREAM_LENGTH_PRESENT;
		expect_exact("smooth", &report, want, 1, 0, n);
		if (report.sample_bytes || samples[0] != 0xA5)
			fail("smooth samples");
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
	test_smooth_double_unsupported();
	test_discard_and_bands();
	test_truncation_checksum_fields();
	test_key_version_trailing();
	test_version_mutation();
	if (g_failures)
		fprintf(stderr, "stream: %d failure(s)\n", g_failures);
	return g_failures ? 1 : 0;
}
