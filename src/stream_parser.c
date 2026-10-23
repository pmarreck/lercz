#include "Lerc_stream.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Running Fletcher32 as Lerc2 computes it: pairs of bytes, a reduction
 * every 359 words, and one more reduction after a trailing odd byte.
 * The differential test against lerc_decode is the check that this matches.
 */
typedef struct {
	uint32_t sum1;
	uint32_t sum2;
	uint32_t words;
	int odd_pending;
	unsigned char odd;
} Fletcher;

static void fletcher_init(Fletcher *f)
{
	f->sum1 = 0xffff;
	f->sum2 = 0xffff;
	f->words = 0;
	f->odd_pending = 0;
}

static void fletcher_reduce(Fletcher *f)
{
	f->sum1 = (f->sum1 & 0xffffu) + (f->sum1 >> 16);
	f->sum2 = (f->sum2 & 0xffffu) + (f->sum2 >> 16);
}

static void fletcher_word(Fletcher *f, unsigned char hi, unsigned char lo)
{
	f->sum1 += (uint32_t)hi << 8;
	f->sum1 += lo;
	f->sum2 += f->sum1;
	if (++f->words == 359) {
		fletcher_reduce(f);
		f->words = 0;
	}
}

static void fletcher_add(Fletcher *f, const unsigned char *p, size_t n)
{
	size_t i = 0;
	if (f->odd_pending && n) {
		fletcher_word(f, f->odd, p[0]);
		f->odd_pending = 0;
		i = 1;
	}
	for (; i + 1 < n; i += 2)
		fletcher_word(f, p[i], p[i + 1]);
	if (i < n) {
		f->odd = p[i];
		f->odd_pending = 1;
	}
}

static uint32_t fletcher_finish(Fletcher *f)
{
	if (f->words)
		fletcher_reduce(f);
	if (f->odd_pending)
		f->sum2 += f->sum1 += (uint32_t)f->odd << 8;
	fletcher_reduce(f);
	return (f->sum2 << 16) | f->sum1;
}

typedef struct {
	const lerc_stream_reader *in;
	lerc_stream_report *report;
	Fletcher sum;
	uint64_t pos;
	int summing;
	int eos;
	int fault;
	int stopped;
	int has_unread;
	unsigned char unread;
	int publishing;
	unsigned char *prev_bits;
	size_t prev_bytes;
	int prev_rows;
	int prev_cols;
} Cur;

static void add_finding(Cur *c, uint64_t offset, uint64_t length, int with_length, uint32_t code, uint32_t category)
{
	lerc_stream_report *r = c->report;
	if (r->findings && r->findings_count < r->findings_capacity) {
		lerc_stream_finding *f = &r->findings[r->findings_count++];
		f->offset = offset;
		f->length = with_length ? length : 0;
		f->code = code;
		f->category = category;
		f->flags = LERC_STREAM_OFFSET_EXACT;
		if (with_length)
			f->flags |= LERC_STREAM_LENGTH_PRESENT;
	} else {
		r->findings_omitted++;
	}
}

static int read_some(Cur *c, unsigned char *dst, size_t n, size_t *got)
{
	*got = 0;
	while (*got < n) {
		size_t chunk = 0;
		size_t want;
		int rc;
		if (c->has_unread) {
			dst[(*got)++] = c->unread;
			c->has_unread = 0;
			continue;
		}
		want = n - *got;
		rc = c->in->read(c->in->ctx, dst + *got, want, &chunk);
		if (rc) {
			c->fault = 1;
			return 0;
		}
		if (chunk == 0) {
			c->eos = 1;
			break;
		}
		if (chunk > want) {
			c->fault = 1;
			return 0;
		}
		*got += chunk;
	}
	if (c->summing && *got)
		fletcher_add(&c->sum, dst, *got);
	c->pos += *got;
	return 1;
}

static void unread_byte(Cur *c, unsigned char b)
{
	c->unread = b;
	c->has_unread = 1;
	c->pos--;
}

static void note_truncated(Cur *c, uint64_t offset, uint64_t length)
{
	add_finding(c, offset, length, 1, LERC_STREAM_CODE_TRUNCATED, LERC_STREAM_CAT_FORMAT);
	c->stopped = 1;
}

static void note_reader(Cur *c)
{
	add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_READER, LERC_STREAM_CAT_IO);
	c->stopped = 1;
}

static int read_exact(Cur *c, unsigned char *dst, size_t n, uint64_t *at)
{
	size_t got = 0;
	uint64_t start = c->pos;
	if (at)
		*at = start;
	if (!read_some(c, dst, n, &got)) {
		note_reader(c);
		return 0;
	}
	if (got != n) {
		note_truncated(c, start + got, (uint64_t)(n - got));
		return 0;
	}
	return 1;
}

static int read_capped(Cur *c, unsigned char *dst, size_t n, uint64_t band_end, uint64_t *at)
{
	uint64_t room;
	if (at)
		*at = c->pos;
	if (c->pos > band_end) {
		c->stopped = 1;
		return 0;
	}
	room = band_end - c->pos;
	if (room < n) {
		unsigned char scratch[8];
		size_t take = room > sizeof(scratch) ? sizeof(scratch) : (size_t)room;
		size_t got = 0;
		unsigned char *buf = n <= sizeof(scratch) ? scratch : dst;
		if (take && n <= sizeof(scratch))
			buf = scratch;
		if (take > n)
			take = n;
		if (take && !read_some(c, buf, take, &got)) {
			note_reader(c);
			return 0;
		}
		note_truncated(c, c->pos, n - got);
		return 0;
	}
	return read_exact(c, dst, n, at);
}

static int read_i32(Cur *c, int32_t *out, uint64_t band_end, int capped, uint64_t *at)
{
	unsigned char b[4];
	uint64_t start = 0;
	int ok = capped ? read_capped(c, b, 4, band_end, &start) : read_exact(c, b, 4, &start);
	if (!ok)
		return 0;
	if (at)
		*at = start;
	*out = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
	return 1;
}

static int read_u32(Cur *c, uint32_t *out, uint64_t *at)
{
	int32_t v = 0;
	if (!read_i32(c, &v, 0, 0, at))
		return 0;
	*out = (uint32_t)v;
	return 1;
}

static int elem_size(int dt)
{
	switch (dt) {
	case 0:
	case 1:
		return 1;
	case 2:
	case 3:
		return 2;
	case 4:
	case 5:
	case 6:
		return 4;
	case 7:
		return 8;
	default:
		return 0;
	}
}

static double load_double(int dt, const unsigned char *p)
{
	switch (dt) {
	case 0: {
		int8_t v;
		memcpy(&v, p, 1);
		return (double)v;
	}
	case 1: {
		uint8_t v;
		memcpy(&v, p, 1);
		return (double)v;
	}
	case 2: {
		int16_t v;
		memcpy(&v, p, 2);
		return (double)v;
	}
	case 3: {
		uint16_t v;
		memcpy(&v, p, 2);
		return (double)v;
	}
	case 4: {
		int32_t v;
		memcpy(&v, p, 4);
		return (double)v;
	}
	case 5: {
		uint32_t v;
		memcpy(&v, p, 4);
		return (double)v;
	}
	case 6: {
		float v;
		memcpy(&v, p, 4);
		return (double)v;
	}
	default: {
		double v;
		memcpy(&v, p, 8);
		return v;
	}
	}
}

static void store_cast(int dt, double v, unsigned char *dst)
{
	switch (dt) {
	case 0: {
		int8_t x = (int8_t)v;
		memcpy(dst, &x, 1);
		break;
	}
	case 1: {
		uint8_t x = (uint8_t)v;
		memcpy(dst, &x, 1);
		break;
	}
	case 2: {
		int16_t x = (int16_t)v;
		memcpy(dst, &x, 2);
		break;
	}
	case 3: {
		uint16_t x = (uint16_t)v;
		memcpy(dst, &x, 2);
		break;
	}
	case 4: {
		int32_t x = (int32_t)v;
		memcpy(dst, &x, 4);
		break;
	}
	case 5: {
		uint32_t x = (uint32_t)v;
		memcpy(dst, &x, 4);
		break;
	}
	case 6: {
		float x = (float)v;
		memcpy(dst, &x, 4);
		break;
	}
	default:
		memcpy(dst, &v, 8);
		break;
	}
}

static int mul_size(size_t a, size_t b, size_t *out)
{
	if (a != 0 && b > SIZE_MAX / a)
		return 0;
	*out = a * b;
	return 1;
}

static int drain(Cur *c, uint64_t band_end)
{
	unsigned char buf[256];
	while (c->pos < band_end) {
		size_t want = (size_t)(band_end - c->pos);
		size_t got = 0;
		if (want > sizeof(buf))
			want = sizeof(buf);
		if (!read_some(c, buf, want, &got)) {
			note_reader(c);
			return 0;
		}
		if (got != want) {
			note_truncated(c, c->pos, band_end - c->pos);
			return 0;
		}
	}
	return 1;
}

static int finish_checksum(Cur *c, int version, uint32_t expected, uint64_t band_start, uint64_t band_end, int *withhold)
{
	if (version < 3)
		return 1;
	if (c->pos != band_end)
		return 1;
	if (fletcher_finish(&c->sum) != expected) {
		add_finding(c, band_start + 10, 4, 1, LERC_STREAM_CODE_CHECKSUM, LERC_STREAM_CAT_CODEC);
		*withhold = 1;
	}
	return 1;
}

static void publish(Cur *c, const unsigned char *band, size_t n, int withhold)
{
	lerc_stream_report *r = c->report;
	if (withhold || !c->publishing)
		c->publishing = 0;
	if (!c->publishing || (r->options & LERC_STREAM_DISCARD) || n == 0)
		return;
	if (r->sample_bytes > SIZE_MAX - n || r->sample_bytes + n > r->sample_capacity) {
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		c->publishing = 0;
		return;
	}
	memcpy(r->samples + r->sample_bytes, band, n);
	r->sample_bytes += n;
}

static int bit_on(const unsigned char *bits, size_t k);

static void publish_mask(Cur *c, const unsigned char *bits, int num_valid, int rows, int cols, int withhold)
{
	lerc_stream_report *r = c->report;
	size_t n = (size_t)rows * (size_t)cols;
	size_t i;
	if (withhold || !c->publishing) {
		c->publishing = 0;
		return;
	}
	if ((r->options & LERC_STREAM_DISCARD) || !r->mask || n == 0)
		return;
	if (r->mask_bytes > SIZE_MAX - n || r->mask_bytes + n > r->mask_capacity) {
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		c->publishing = 0;
		return;
	}
	for (i = 0; i < n; i++)
		r->mask[r->mask_bytes + i] = (num_valid == 0 || (bits && !bit_on(bits, i))) ? 0 : 1;
	r->mask_bytes += n;
}

static int band_too_big(int rows, int cols, int depth, int dt, uint64_t *where_kind)
{
	int es;
	long long pixel;
	long long bytes;
	if (rows <= 0 || cols <= 0 || depth <= 0)
		return 0;
	if (rows > INT_MAX / cols) {
		*where_kind = 1;
		return 1;
	}
	pixel = (long long)rows * cols;
	if (pixel > INT_MAX) {
		*where_kind = 1;
		return 1;
	}
	es = elem_size(dt);
	if (es <= 0)
		return 0;
	if ((long long)es * depth > INT_MAX) {
		*where_kind = 2;
		return 1;
	}
	bytes = (long long)es * depth * pixel;
	if (bytes > INT_MAX) {
		*where_kind = 2;
		return 1;
	}
	return 0;
}

static int bit_on(const unsigned char *bits, size_t k)
{
	return (bits[k >> 3] & (unsigned char)(0x80u >> (k & 7))) != 0;
}

static size_t count_valid(const unsigned char *bits, size_t npix)
{
	size_t i, n = 0;
	if (!bits)
		return npix;
	for (i = 0; i < npix; i++)
		if (bit_on(bits, i))
			n++;
	return n;
}

/* Expand LERC's byte run-length stream. Counts are little-endian int16.
 * A positive count copies that many literals. A negative count repeats the
 * next byte. The terminator is -32768. Success requires dst_n output bytes.
 */
static int rle_decode(const unsigned char *src, size_t n, unsigned char *dst, size_t dst_n, size_t *used)
{
	size_t i = 0;
	size_t o = 0;
	while (i + 2 <= n) {
		int16_t cnt;
		memcpy(&cnt, src + i, 2);
		i += 2;
		if (cnt == (int16_t)-32768) {
			*used = i;
			return o == dst_n;
		}
		if (cnt > 0) {
			size_t lit = (size_t)cnt;
			if (lit > n - i || o > dst_n || lit > dst_n - o)
				return 0;
			memcpy(dst + o, src + i, lit);
			i += lit;
			o += lit;
		} else {
			size_t rep = (size_t)(-(int)cnt);
			if (i >= n || o > dst_n || rep > dst_n - o)
				return 0;
			if (rep)
				memset(dst + o, src[i], rep);
			i += 1;
			o += rep;
		}
	}
	return 0;
}

static int fill_const(int dt, int rows, int cols, int depth, int num_valid, double zmin, int per_depth,
	const double *per_min, const unsigned char *bits, unsigned char *dst)
{
	int es = elem_size(dt);
	size_t pixel = (size_t)rows * (size_t)cols;
	size_t i;
	memset(dst, 0, pixel * (size_t)depth * (size_t)es);
	if (num_valid <= 0)
		return 1;
	for (i = 0; i < pixel; i++) {
		int d;
		unsigned char *pix;
		if (bits && !bit_on(bits, i))
			continue;
		pix = dst + i * (size_t)depth * (size_t)es;
		if (!per_depth) {
			for (d = 0; d < depth; d++)
				store_cast(dt, zmin, pix + (size_t)d * (size_t)es);
		} else {
			for (d = 0; d < depth; d++)
				store_cast(dt, per_min[d], pix + (size_t)d * (size_t)es);
		}
	}
	return 1;
}

static int scatter_valid(const unsigned char *bits, size_t npix, int depth, int es,
	const unsigned char *raw, size_t raw_n, unsigned char *dst)
{
	size_t step = (size_t)depth * (size_t)es;
	size_t k, used = 0;
	memset(dst, 0, npix * step);
	for (k = 0; k < npix; k++) {
		if (bits && !bit_on(bits, k))
			continue;
		if (used > raw_n || step > raw_n - used)
			return 0;
		memcpy(dst + k * step, raw + used, step);
		used += step;
	}
	return used == raw_n;
}

static int parse_band(Cur *c)
{
	static const char key[] = "Lerc2 ";
	uint64_t band_start = c->pos;
	uint64_t band_end = 0;
	uint64_t at = 0;
	uint64_t rows_at = 0, cols_at = 0, depth_at = 0, valid_at = 0, micro_at = 0, blob_at = 0, dt_at = 0;
	uint32_t mark;
	uint32_t expected_sum = 0;
	int32_t version = 0, rows = 0, cols = 0, depth = 1, num_valid = 0, micro = 0, blob_size = 0, dt = -1;
	int32_t blobs_more = 0;
	int n_ints, n_dbls, i;
	int fatal = 0;
	int withhold = 0;
	int es;
	double zmin = 0, zmax = 0;
	unsigned char *samples = NULL;
	unsigned char *bits = NULL;
	unsigned char *ranges = NULL;
	double *dmin = NULL;
	double *dmax = NULL;
	size_t sample_bytes = 0;
	int rc = 0;
	int mask_ok = 0;

	fletcher_init(&c->sum);
	c->summing = 0;
	mark = c->report->findings_count + c->report->findings_omitted;

	for (i = 0; i < 6; i++) {
		unsigned char b = 0;
		if (!read_exact(c, &b, 1, &at))
			return 0;
		if (b != (unsigned char)key[i]) {
			add_finding(c, at, 1, 1, LERC_STREAM_CODE_FILE_KEY, LERC_STREAM_CAT_FORMAT);
			c->stopped = 1;
			return 0;
		}
	}
	if (!read_i32(c, &version, 0, 0, &at))
		return 0;
	if (version < 0 || version > 6) {
		add_finding(c, at, 4, 1, LERC_STREAM_CODE_VERSION, LERC_STREAM_CAT_FORMAT);
		c->stopped = 1;
		return 0;
	}
	if (version >= 3) {
		if (!read_u32(c, &expected_sum, &at))
			return 0;
		c->summing = 1;
	}

	n_ints = 6 + (version >= 4 ? 1 : 0) + (version >= 6 ? 1 : 0);
	for (i = 0; i < n_ints; i++) {
		int32_t v = 0;
		uint64_t field = 0;
		if (!read_i32(c, &v, 0, 0, &field))
			return 0;
		switch (i) {
		case 0:
			rows = v;
			rows_at = field;
			break;
		case 1:
			cols = v;
			cols_at = field;
			break;
		default:
			break;
		}
		if (version >= 4 && i == 2) {
			depth = v;
			depth_at = field;
		}
		if (i == (version >= 4 ? 3 : 2)) {
			num_valid = v;
			valid_at = field;
		}
		if (i == (version >= 4 ? 4 : 3)) {
			micro = v;
			micro_at = field;
		}
		if (i == (version >= 4 ? 5 : 4)) {
			blob_size = v;
			blob_at = field;
		}
		if (i == (version >= 4 ? 6 : 5)) {
			dt = v;
			dt_at = field;
		}
		if (version >= 6 && i == 7)
			blobs_more = v;
	}
	if (version >= 6) {
		unsigned char flags[4];
		if (!read_exact(c, flags, 4, NULL))
			return 0;
	}
	n_dbls = 3 + (version >= 6 ? 2 : 0);
	for (i = 0; i < n_dbls; i++) {
		unsigned char raw[8];
		double v = 0;
		if (!read_exact(c, raw, 8, NULL))
			return 0;
		memcpy(&v, raw, 8);
		if (i == 1)
			zmin = v;
		if (i == 2)
			zmax = v;
	}

	if (rows <= 0) {
		add_finding(c, rows_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}
	if (cols <= 0) {
		add_finding(c, cols_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}
	if (depth <= 0) {
		add_finding(c, depth_at ? depth_at : band_start, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}
	if (num_valid < 0) {
		add_finding(c, valid_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}
	if (micro <= 0) {
		add_finding(c, micro_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	} else if (micro > 32) {
		add_finding(c, micro_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_LIMIT);
		fatal = 1;
	}
	if (blob_size <= 0) {
		add_finding(c, blob_at, 4, 1, LERC_STREAM_CODE_BLOB_EXTENT, LERC_STREAM_CAT_FORMAT);
		c->stopped = 1;
		return 0;
	}
	if (dt < 0 || dt > 7) {
		add_finding(c, dt_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}
	if (!fatal && rows > 0 && cols > 0) {
		long long pixel = (long long)rows * (long long)cols;
		uint64_t kind = 0;
		if (rows > INT_MAX / cols || pixel > INT_MAX) {
			add_finding(c, cols_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_LIMIT);
			fatal = 1;
		} else if (num_valid > pixel) {
			add_finding(c, valid_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
			fatal = 1;
		} else if (band_too_big(rows, cols, depth, dt, &kind)) {
			add_finding(c, kind == 1 ? cols_at : depth_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_LIMIT);
			fatal = 1;
		}
	} else if (num_valid > 0) {
		add_finding(c, valid_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_FORMAT);
		fatal = 1;
	}

	if ((uint64_t)blob_size < c->pos - band_start) {
		add_finding(c, blob_at, 4, 1, LERC_STREAM_CODE_BLOB_EXTENT, LERC_STREAM_CAT_FORMAT);
		c->stopped = 1;
		return 0;
	}
	band_end = band_start + (uint64_t)blob_size;
	es = elem_size(dt);

	if (!fatal && es > 0 && rows > 0 && cols > 0 && depth > 0) {
		size_t pixels = 0, depth_bytes = 0;
		if (!mul_size((size_t)rows, (size_t)cols, &pixels) ||
			!mul_size(pixels, (size_t)depth, &depth_bytes) ||
			!mul_size(depth_bytes, (size_t)es, &sample_bytes)) {
			add_finding(c, rows_at, 4, 1, LERC_STREAM_CODE_HEADER_FIELD, LERC_STREAM_CAT_LIMIT);
			fatal = 1;
		}
	}

	if (fatal) {
		withhold = 1;
		if (!drain(c, band_end))
			return 0;
		if (!finish_checksum(c, version, expected_sum, band_start, band_end, &withhold))
			return 0;
		c->publishing = 0;
		(void)mark;
		rc = 1;
		goto done;
	}

	{
		int32_t mask_bytes = 0;
		uint64_t mask_at = 0;
		int all_valid = num_valid == rows * cols;
		int all_invalid = num_valid == 0;
		if (!read_i32(c, &mask_bytes, band_end, 1, &mask_at))
			return 0;
		if (mask_bytes < 0 || ((all_valid || all_invalid) && mask_bytes != 0)) {
			add_finding(c, mask_at, 4, 1, LERC_STREAM_CODE_MASK, LERC_STREAM_CAT_FORMAT);
			withhold = 1;
			if (!drain(c, band_end))
				return 0;
		} else if (!all_valid && !all_invalid && mask_bytes == 0) {
			size_t nbytes = ((size_t)rows * (size_t)cols + 7) >> 3;
			if (!c->prev_bits || c->prev_rows != rows || c->prev_cols != cols || c->prev_bytes != nbytes) {
				add_finding(c, mask_at, 4, 1, LERC_STREAM_CODE_MASK, LERC_STREAM_CAT_FORMAT);
				withhold = 1;
				if (!drain(c, band_end))
					return 0;
			} else {
				bits = malloc(nbytes ? nbytes : 1);
				if (!bits) {
					add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
					withhold = 1;
					if (!drain(c, band_end)) {
						rc = 0;
						goto done;
					}
				} else {
					memcpy(bits, c->prev_bits, nbytes);
					mask_ok = 1;
				}
			}
		} else if (mask_bytes > 0) {
			uint64_t room = c->pos <= band_end ? band_end - c->pos : 0;
			unsigned char *packed = NULL;
			size_t npix = (size_t)rows * (size_t)cols;
			size_t nbytes = (npix + 7) >> 3;
			size_t used = 0;
			if ((uint64_t)mask_bytes > room) {
				add_finding(c, c->pos, room, 1, LERC_STREAM_CODE_TRUNCATED, LERC_STREAM_CAT_FORMAT);
				c->stopped = 1;
				return 0;
			}
			packed = malloc((size_t)mask_bytes);
			bits = calloc(1, nbytes ? nbytes : 1);
			if (!packed || !bits) {
				add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
				withhold = 1;
				free(packed);
				if (!drain(c, band_end)) {
					rc = 0;
					goto done;
				}
			} else if (!read_capped(c, packed, (size_t)mask_bytes, band_end, NULL)) {
				free(packed);
				rc = 0;
				goto done;
			} else if (!rle_decode(packed, (size_t)mask_bytes, bits, nbytes, &used)) {
				free(packed);
				add_finding(c, mask_at, 4, 1, LERC_STREAM_CODE_MASK, LERC_STREAM_CAT_FORMAT);
				withhold = 1;
				if (!drain(c, band_end)) {
					rc = 0;
					goto done;
				}
			} else {
				free(packed);
				mask_ok = 1;
			}
		} else {
			mask_ok = 1;
		}
		if (!withhold && (num_valid == 0 || zmin == zmax)) {
			if ((c->report->options & LERC_STREAM_DISCARD) == 0 && c->publishing) {
				samples = calloc(1, sample_bytes ? sample_bytes : 1);
				if (!samples) {
					add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
					withhold = 1;
				} else {
					fill_const(dt, rows, cols, depth, num_valid, zmin, 0, NULL, bits, samples);
				}
			}
		} else if (!withhold && version >= 4) {
			size_t range_one = (size_t)depth * (size_t)es;
			int equal = 1;
			ranges = malloc(range_one * 2);
			dmin = calloc((size_t)depth, sizeof(double));
			dmax = calloc((size_t)depth, sizeof(double));
			if (!ranges || !dmin || !dmax) {
				add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
				withhold = 1;
				if (!drain(c, band_end)) {
					rc = 0;
					goto done;
				}
			} else if ((uint64_t)range_one * 2 > band_end - c->pos) {
				add_finding(c, c->pos, band_end - c->pos, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
				withhold = 1;
				if (!drain(c, band_end)) {
					rc = 0;
					goto done;
				}
			} else if (!read_capped(c, ranges, range_one * 2, band_end, &at)) {
				rc = 0;
				goto done;
			} else {
				for (i = 0; i < depth; i++) {
					dmin[i] = load_double(dt, ranges + (size_t)i * (size_t)es);
					dmax[i] = load_double(dt, ranges + range_one + (size_t)i * (size_t)es);
					if (memcmp(&dmin[i], &dmax[i], sizeof(double)) != 0)
						equal = 0;
				}
				if (equal) {
					if ((c->report->options & LERC_STREAM_DISCARD) == 0 && c->publishing) {
						samples = calloc(1, sample_bytes ? sample_bytes : 1);
						if (!samples) {
							add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
							withhold = 1;
						} else {
							fill_const(dt, rows, cols, depth, num_valid, zmin, 1, dmin, bits, samples);
						}
					}
				} else {
					unsigned char flag = 0;
					if (!read_capped(c, &flag, 1, band_end, &at)) {
						rc = 0;
						goto done;
					}
					if (flag == 0) {
						add_finding(c, at, 1, 1, LERC_STREAM_CODE_UNSUPPORTED, LERC_STREAM_CAT_UNSUPPORTED);
						withhold = 1;
						if (!drain(c, band_end)) {
							rc = 0;
							goto done;
						}
					} else {
						size_t raw_n = 0;
						size_t npix = (size_t)rows * (size_t)cols;
						size_t valid_n = bits ? count_valid(bits, npix) : (size_t)num_valid;
						if (!mul_size(valid_n, (size_t)depth * (size_t)es, &raw_n) ||
							(uint64_t)raw_n > band_end - c->pos) {
							add_finding(c, c->pos, band_end > c->pos ? band_end - c->pos : 0, 1,
								LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
							withhold = 1;
							if (!drain(c, band_end)) {
								rc = 0;
								goto done;
							}
						} else if ((c->report->options & LERC_STREAM_DISCARD) || !c->publishing) {
							if (!drain(c, c->pos + raw_n)) {
								rc = 0;
								goto done;
							}
						} else if (!bits) {
							samples = calloc(1, sample_bytes ? sample_bytes : 1);
							if (!samples) {
								add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
								withhold = 1;
								if (!drain(c, band_end)) {
									rc = 0;
									goto done;
								}
							} else if (!read_capped(c, samples, raw_n, band_end, NULL)) {
								rc = 0;
								goto done;
							}
						} else {
							unsigned char *raw = malloc(raw_n ? raw_n : 1);
							samples = calloc(1, sample_bytes ? sample_bytes : 1);
							if (!raw || !samples) {
								free(raw);
								add_finding(c, band_start, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
								withhold = 1;
								if (!drain(c, band_end)) {
									rc = 0;
									goto done;
								}
							} else if (!read_capped(c, raw, raw_n, band_end, NULL)) {
								free(raw);
								rc = 0;
								goto done;
							} else if (!scatter_valid(bits, npix, depth, es, raw, raw_n, samples)) {
								free(raw);
								add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
								withhold = 1;
							} else {
								free(raw);
							}
						}
					}
				}
			}
		} else if (!withhold) {
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_UNSUPPORTED, LERC_STREAM_CAT_UNSUPPORTED);
			withhold = 1;
			if (!drain(c, band_end))
				return 0;
		}
	}

	if (c->pos < band_end) {
		add_finding(c, c->pos, band_end - c->pos, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		withhold = 1;
		if (!drain(c, band_end)) {
			rc = 0;
			goto done;
		}
	} else if (c->pos > band_end) {
		add_finding(c, band_end, c->pos - band_end, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		c->stopped = 1;
		withhold = 1;
		rc = 0;
		goto done;
	}
	if (!finish_checksum(c, version, expected_sum, band_start, band_end, &withhold)) {
		rc = 0;
		goto done;
	}
	if (c->report->findings_count + c->report->findings_omitted != mark)
		withhold = 1;
	if (!withhold && c->publishing && (c->report->options & LERC_STREAM_DISCARD) == 0 && c->report->mask) {
		size_t n = (size_t)rows * (size_t)cols;
		if (c->report->mask_bytes > SIZE_MAX - n || c->report->mask_bytes + n > c->report->mask_capacity) {
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
			withhold = 1;
		}
	}
	publish(c, samples, sample_bytes, withhold);
	publish_mask(c, bits, num_valid, rows, cols, withhold);
	if (mask_ok && rows > 0 && cols > 0) {
		size_t nbytes = ((size_t)rows * (size_t)cols + 7) >> 3;
		unsigned char *copy = malloc(nbytes ? nbytes : 1);
		if (copy) {
			if (bits)
				memcpy(copy, bits, nbytes);
			else if (num_valid == 0)
				memset(copy, 0, nbytes);
			else
				memset(copy, 0xff, nbytes);
			free(c->prev_bits);
			c->prev_bits = copy;
			c->prev_rows = rows;
			c->prev_cols = cols;
			c->prev_bytes = nbytes;
		}
	}
	rc = 1;
	(void)blobs_more;
done:
	free(samples);
	free(bits);
	free(ranges);
	free(dmin);
	free(dmax);
	/* Stash blobs_more in the unread-independent report via a side channel: re-read is impossible.
	 * Keep it in Cur by packing into stopped's neighbor. Use summing's neighbor field instead.
	 */
	c->sum.words = (uint32_t)blobs_more;
	c->sum.odd = (unsigned char)version;
	return rc;
}

static int after_band(Cur *c, int version, int blobs_more)
{
	unsigned char b = 0;
	size_t got = 0;
	uint64_t start;
	int save;
	if (c->stopped)
		return 0;
	save = c->summing;
	c->summing = 0;
	start = c->pos;
	if (!read_some(c, &b, 1, &got)) {
		note_reader(c);
		return 0;
	}
	c->summing = save;
	if (!got)
		return 0;
	if ((version < 6 && b == 'L') || (version >= 6 && blobs_more > 0)) {
		unread_byte(c, b);
		return 1;
	}
	{
		uint64_t extra = 1;
		unsigned char buf[256];
		for (;;) {
			if (!read_some(c, buf, sizeof(buf), &got)) {
				note_reader(c);
				return 0;
			}
			if (!got)
				break;
			extra += got;
		}
		add_finding(c, start, extra, 1, LERC_STREAM_CODE_TRAILING, LERC_STREAM_CAT_FORMAT);
	}
	return 0;
}

lerc_status lerc_stream_parse(const lerc_stream_reader *reader, lerc_stream_report *report)
{
	Cur cur;
	int one = 1;
	if (!reader || !reader->read || !report)
		return 2;
	if ((report->options & LERC_STREAM_DISCARD) == 0 && !report->samples)
		return 2;
	if (report->findings_capacity && !report->findings)
		return 2;
	report->sample_bytes = 0;
	report->mask_bytes = 0;
	report->findings_count = 0;
	report->findings_omitted = 0;
	report->bytes_consumed = 0;
	report->stopped = 0;
	memset(&cur, 0, sizeof(cur));
	cur.in = reader;
	cur.report = report;
	cur.publishing = 1;
	if (*(unsigned char *)&one != 1) {
		add_finding(&cur, 0, 0, 0, LERC_STREAM_CODE_UNSUPPORTED, LERC_STREAM_CAT_UNSUPPORTED);
		cur.stopped = 1;
		report->stopped = 1;
		return 0;
	}
	while (!cur.stopped) {
		int version;
		int blobs_more;
		if (!parse_band(&cur))
			break;
		version = (int)cur.sum.odd;
		blobs_more = (int)cur.sum.words;
		if (!after_band(&cur, version, blobs_more))
			break;
	}
	free(cur.prev_bits);
	report->bytes_consumed = cur.pos;
	report->stopped = (uint32_t)cur.stopped;
	return 0;
}

int lerc_stream_memory_read(void *ctx, unsigned char *dst, size_t n, size_t *n_out)
{
	lerc_stream_memory *m = ctx;
	size_t avail;
	size_t take;
	if (!m || !n_out || (n && !dst))
		return 1;
	avail = m->pos < m->size ? m->size - m->pos : 0;
	take = n < avail ? n : avail;
	if (take)
		memcpy(dst, m->data + m->pos, take);
	m->pos += take;
	*n_out = take;
	return 0;
}
