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

/* Forward-only Lerc2 tiles. Bit fields are unpacked LSB-first inside each
 * little-endian uint32, matching Lerc2 v3 and later. The spanning shift
 * stays on uint32: a wider word would keep bits that shift drops.
 */

static unsigned int tail_unused(unsigned int n_elem, int num_bits)
{
	unsigned int bits_tail = (unsigned int)(((unsigned long long)n_elem * (unsigned int)num_bits) & 31u);
	unsigned int bytes_tail = (bits_tail + 7u) >> 3;
	return bytes_tail > 0 ? 4u - bytes_tail : 0u;
}

static int pull_uint(Cur *c, uint64_t band_end, int nb, unsigned int *out)
{
	unsigned char b[4];
	if (nb != 1 && nb != 2 && nb != 4)
		return 0;
	if (!read_capped(c, b, (size_t)nb, band_end, NULL))
		return -1;
	if (nb == 1)
		*out = b[0];
	else if (nb == 2)
		*out = (unsigned int)b[0] | ((unsigned int)b[1] << 8);
	else {
		unsigned int v;
		memcpy(&v, b, 4);
		*out = v;
	}
	return 1;
}

/* LSB-first unpack of one Lerc2 v3 bit field. num_bits is in 1..31. */
static int unstuff_pull(Cur *c, uint64_t band_end, unsigned int *dst, unsigned int n_elem, int num_bits)
{
	unsigned long long nuints_ll;
	size_t nuints, nbytes, used;
	unsigned int tail;
	unsigned char *raw = NULL;
	unsigned int *words = NULL;
	unsigned int *sp;
	unsigned int i;
	int bit_pos, nb;
	if (n_elem == 0 || num_bits <= 0 || num_bits >= 32)
		return 0;
	nuints_ll = ((unsigned long long)n_elem * (unsigned int)num_bits + 31ull) / 32ull;
	if (nuints_ll > SIZE_MAX / 4)
		return 0;
	nuints = (size_t)nuints_ll;
	nbytes = nuints * 4;
	tail = tail_unused(n_elem, num_bits);
	if ((size_t)tail > nbytes)
		return 0;
	used = nbytes - tail;
	raw = malloc(used ? used : 1);
	words = calloc(nuints, 4);
	if (!raw || !words) {
		free(raw);
		free(words);
		return -2;
	}
	if (!read_capped(c, raw, used, band_end, NULL)) {
		free(raw);
		free(words);
		return -1;
	}
	memcpy(words, raw, used);
	free(raw);
	bit_pos = 0;
	nb = 32 - num_bits;
	sp = words;
	for (i = 0; i < n_elem; i++) {
		if (nb - bit_pos >= 0) {
			unsigned int w = *sp;
			dst[i] = (w << (unsigned)(nb - bit_pos)) >> (unsigned)nb;
			bit_pos += num_bits;
			if (bit_pos == 32) {
				sp++;
				bit_pos = 0;
			}
		} else {
			unsigned int v = (*sp++) >> (unsigned)bit_pos;
			int sh = 64 - num_bits - bit_pos;
			/* The 64 in the formula cancels to a shift of 1..31 on a uint32.
			 * Widening the word would keep bits this shift is meant to drop.
			 */
			if (sh < 0 || sh >= 32) {
				free(words);
				return 0;
			}
			v |= ((*sp) << (unsigned)sh) >> (unsigned)nb;
			dst[i] = v;
			bit_pos -= nb;
		}
	}
	free(words);
	return 1;
}

/* BitStuffer2::Decode for version >= 3, including the LUT form.
 * Returns 1, 0 with one consumption finding, -1 if the reader already
 * recorded a fault, or -2 on allocation failure.
 */
static int bitstuff_pull(Cur *c, uint64_t band_end, unsigned int *dst, size_t max_count, size_t *n_out)
{
	unsigned char hdr = 0;
	int bits67, nb, lut, num_bits, rc;
	unsigned int n_elem = 0;
	uint64_t hdr_at = 0, count_at;
	if (!read_capped(c, &hdr, 1, band_end, &hdr_at))
		return -1;
	bits67 = hdr >> 6;
	nb = bits67 == 0 ? 4 : 3 - bits67;
	lut = (hdr >> 5) & 1;
	num_bits = hdr & 31;
	if (nb != 1 && nb != 2 && nb != 4) {
		add_finding(c, hdr_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		return 0;
	}
	count_at = c->pos;
	rc = pull_uint(c, band_end, nb, &n_elem);
	if (rc != 1)
		return rc;
	if ((size_t)n_elem > max_count) {
		add_finding(c, count_at, (uint64_t)nb, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		return 0;
	}
	if (!lut) {
		if (num_bits == 0) {
			if (n_elem)
				memset(dst, 0, (size_t)n_elem * sizeof(unsigned int));
			*n_out = n_elem;
			return 1;
		}
		rc = unstuff_pull(c, band_end, dst, n_elem, num_bits);
		if (rc == 0)
			add_finding(c, count_at, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		if (rc == 1)
			*n_out = n_elem;
		return rc;
	}
	if (num_bits == 0) {
		add_finding(c, count_at, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		return 0;
	}
	{
		unsigned char lut_n = 0;
		unsigned int n_lut, i;
		unsigned int *lutv = NULL;
		int nbits = 0;
		uint64_t lut_at;
		if (!read_capped(c, &lut_n, 1, band_end, &lut_at))
			return -1;
		if (lut_n == 0) {
			add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return 0;
		}
		n_lut = (unsigned int)lut_n - 1u;
		if (n_lut == 0) {
			add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return 0;
		}
		lutv = calloc((size_t)n_lut + 1u, sizeof(unsigned int));
		if (!lutv)
			return -2;
		rc = unstuff_pull(c, band_end, lutv + 1, n_lut, num_bits);
		if (rc != 1) {
			free(lutv);
			if (rc == 0)
				add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return rc;
		}
		while ((n_lut >> nbits) != 0)
			nbits++;
		if (nbits <= 0 || nbits >= 32) {
			free(lutv);
			add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return 0;
		}
		rc = unstuff_pull(c, band_end, dst, n_elem, nbits);
		if (rc != 1) {
			free(lutv);
			if (rc == 0)
				add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return rc;
		}
		for (i = 0; i < n_elem; i++) {
			if (dst[i] > n_lut) {
				free(lutv);
				add_finding(c, lut_at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
				return 0;
			}
			dst[i] = lutv[dst[i]];
		}
		free(lutv);
		*n_out = n_elem;
		return 1;
	}
}

static int dtype_used(int dt, int tc)
{
	int r = dt;
	switch (dt) {
	case 2:
	case 4:
		r = dt - tc;
		break;
	case 3:
	case 5:
		r = dt - 2 * tc;
		break;
	case 6:
		if (tc == 0)
			return 6;
		return tc == 1 ? 2 : 1;
	case 7:
		if (tc == 0)
			return 7;
		r = dt - 2 * tc + 1;
		break;
	default:
		return dt;
	}
	if (r < 0 || r > 7)
		return -1;
	return r;
}

static int pixel_on(const unsigned char *bits, int cols, int row, int col)
{
	if (!bits)
		return 1;
	return bit_on(bits, (size_t)row * (size_t)cols + (size_t)col);
}

static unsigned char *pix_at(unsigned char *img, int cols, int depth, int es, int row, int col, int idepth)
{
	size_t m = ((size_t)row * (size_t)cols + (size_t)col) * (size_t)depth + (size_t)idepth;
	return img + m * (size_t)es;
}

static size_t tile_valid(const unsigned char *bits, int cols, int i0, int i1, int j0, int j1)
{
	int i, j;
	size_t n = 0;
	for (i = i0; i < i1; i++)
		for (j = j0; j < j1; j++)
			if (pixel_on(bits, cols, i, j))
				n++;
	return n;
}

static void store_clamped(int dt, double offset, unsigned int quant, double inv, double clamp, int diff,
	const unsigned char *prev, unsigned char *dst)
{
	double z = offset + (double)quant * inv;
	if (diff)
		z += load_double(dt, prev);
	if (z > clamp)
		z = clamp;
	store_cast(dt, z, dst);
}

/* One tile, one depth. img may be NULL when the pixels are dropped.
 * Same return codes as bitstuff_pull. A 0 means one finding was added.
 */
static int read_one_tile(Cur *c, uint64_t band_end, int version, int cols, int depth, int dt,
	int i0, int i1, int j0, int j1, int idepth, double zerr, double zmax, const double *dmax,
	const unsigned char *bits, unsigned char *img)
{
	unsigned char flag = 0;
	uint64_t at = 0;
	int pattern, mode, bdiff, bits67, es, dt_used, rc;
	int row, col;
	size_t tw, th, npix, nq = 0;
	double offset = 0, clamp, inv;
	if (!read_capped(c, &flag, 1, band_end, &at))
		return -1;
	pattern = version >= 5 ? 14 : 15;
	if (((flag >> 2) & pattern) != ((j0 >> 3) & pattern) || ((version >= 5) && (flag & 4) && idepth == 0)) {
		add_finding(c, at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		return 0;
	}
	bdiff = (version >= 5) && (flag & 4);
	bits67 = flag >> 6;
	mode = flag & 3;
	es = elem_size(dt);
	th = (size_t)(i1 - i0);
	tw = (size_t)(j1 - j0);
	npix = th * tw;
	if (mode == 2) {
		if (!img)
			return 1;
		for (row = i0; row < i1; row++) {
			for (col = j0; col < j1; col++) {
				unsigned char *dst;
				if (!pixel_on(bits, cols, row, col))
					continue;
				dst = pix_at(img, cols, depth, es, row, col, idepth);
				if (bdiff)
					memcpy(dst, dst - es, (size_t)es);
				else
					store_cast(dt, 0, dst);
			}
		}
		return 1;
	}
	if (mode == 0) {
		if (bdiff) {
			add_finding(c, at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return 0;
		}
		for (row = i0; row < i1; row++) {
			for (col = j0; col < j1; col++) {
				unsigned char tmp[8];
				if (!pixel_on(bits, cols, row, col))
					continue;
				if (!read_capped(c, tmp, (size_t)es, band_end, NULL))
					return -1;
				if (img)
					memcpy(pix_at(img, cols, depth, es, row, col, idepth), tmp, (size_t)es);
			}
		}
		return 1;
	}
	dt_used = dtype_used((bdiff && dt < 6) ? 4 : dt, bits67);
	if (dt_used < 0 || elem_size(dt_used) <= 0) {
		add_finding(c, at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		return 0;
	}
	{
		unsigned char raw[8];
		int n = elem_size(dt_used);
		if (!read_capped(c, raw, (size_t)n, band_end, NULL))
			return -1;
		offset = load_double(dt_used, raw);
	}
	clamp = (version >= 4 && depth > 1 && dmax) ? dmax[idepth] : zmax;
	if (mode == 3) {
		if (!img)
			return 1;
		for (row = i0; row < i1; row++) {
			for (col = j0; col < j1; col++) {
				unsigned char *dst;
				if (!pixel_on(bits, cols, row, col))
					continue;
				dst = pix_at(img, cols, depth, es, row, col, idepth);
				if (!bdiff)
					store_cast(dt, offset, dst);
				else {
					double z = offset + load_double(dt, dst - es);
					if (z > clamp)
						z = clamp;
					store_cast(dt, z, dst);
				}
			}
		}
		return 1;
	}
	{
		unsigned int *quants = calloc(npix ? npix : 1, sizeof(unsigned int));
		if (!quants)
			return -2;
		rc = bitstuff_pull(c, band_end, quants, npix, &nq);
		if (rc != 1) {
			free(quants);
			return rc;
		}
		inv = 2 * zerr;
		if (img) {
			size_t q = 0;
			int all = nq == npix;
			if (!all && nq != tile_valid(bits, cols, i0, i1, j0, j1)) {
				free(quants);
				add_finding(c, at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
				return 0;
			}
			for (row = i0; row < i1; row++) {
				for (col = j0; col < j1; col++) {
					unsigned char *dst;
					if (!all && !pixel_on(bits, cols, row, col))
						continue;
					dst = pix_at(img, cols, depth, es, row, col, idepth);
					store_clamped(dt, offset, quants[q++], inv, clamp, bdiff, bdiff ? dst - es : dst, dst);
				}
			}
		} else if (nq != npix && nq != tile_valid(bits, cols, i0, i1, j0, j1)) {
			free(quants);
			add_finding(c, at, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			return 0;
		}
		free(quants);
		return 1;
	}
}

/* Walk the microblock grid. Stops at the first tile that does not decode. */
static int read_tiles(Cur *c, uint64_t band_end, int version, int rows, int cols, int depth, int dt, int micro,
	double zerr, double zmax, const double *dmax, const unsigned char *bits, unsigned char *img)
{
	int nvert = (int)(((long long)rows + micro - 1) / micro);
	int nhori = (int)(((long long)cols + micro - 1) / micro);
	int ti, tj, d;
	for (ti = 0; ti < nvert; ti++) {
		int i0 = (int)((long long)ti * micro);
		long long i1ll = (long long)i0 + micro;
		int i1 = i1ll > rows ? rows : (int)i1ll;
		for (tj = 0; tj < nhori; tj++) {
			int j0 = (int)((long long)tj * micro);
			long long j1ll = (long long)j0 + micro;
			int j1 = j1ll > cols ? cols : (int)j1ll;
			for (d = 0; d < depth; d++) {
				int rc = read_one_tile(c, band_end, version, cols, depth, dt, i0, i1, j0, j1, d,
					zerr, zmax, dmax, bits, img);
				if (rc != 1)
					return rc;
			}
		}
	}
	return 1;
}

/* Codes and pixel bits are packed MSB-first in each little-endian uint32.
 * Tile unstuff is the other order; do not reuse it here.
 */
static int huff_peek(const unsigned char *buf, size_t len, size_t off, int bit_pos, int nbits, unsigned int *out)
{
	unsigned int w = 0, nxt = 0;
	int sh;
	if (nbits <= 0 || nbits > 32 || bit_pos < 0 || bit_pos >= 32 || off > len || len - off < 4)
		return 0;
	memcpy(&w, buf + off, 4);
	*out = (w << (unsigned)bit_pos) >> (unsigned)(32 - nbits);
	if (32 - bit_pos >= nbits)
		return 1;
	if (len - off < 8)
		return 0;
	memcpy(&nxt, buf + off + 4, 4);
	sh = 64 - bit_pos - nbits;
	if (sh <= 0 || sh >= 32)
		return 0;
	*out |= nxt >> (unsigned)sh;
	return 1;
}

static void huff_advance(size_t *off, int *bit_pos, int nbits)
{
	if (32 - *bit_pos >= nbits) {
		*bit_pos += nbits;
		if (*bit_pos == 32) {
			*bit_pos = 0;
			*off += 4;
		}
	} else {
		*bit_pos += nbits - 32;
		*off += 4;
	}
}

static int huff_wrap(int i, int size)
{
	return i < size ? i : i - size;
}

/* Byte and char Huffman. Mode 1 is a neighbor delta. Mode 2 is the value.
 * Returns 0 only when the reader already stopped the walk.
 */
static int huffman_pull(Cur *c, uint64_t band_end, int rows, int cols, int depth, int dt, int mode,
	const unsigned char *bits, unsigned char **samples, size_t sample_bytes, int *withhold)
{
	enum { HUFF_MAX = 1 << 15, HUFF_LUT = 12 };
	int32_t hv = 0, hsize = 0, i0 = 0, i1 = 0;
	int nlen, max_len, num_bits, i, k;
	int row, col, d;
	size_t n_out = 0, sum, code_bytes, pay_at, off, stride, m;
	unsigned int *lengths = NULL;
	unsigned short *clen = NULL;
	unsigned int *ccode = NULL;
	unsigned char *coded = NULL;
	unsigned char *pay = NULL;
	unsigned char *img = NULL;
	struct { short len; short sym; } *lut = NULL;
	size_t lut_n, pay_n;
	int bit_pos, rc;
	if (!read_i32(c, &hv, band_end, 1, NULL) || !read_i32(c, &hsize, band_end, 1, NULL) ||
		!read_i32(c, &i0, band_end, 1, NULL) || !read_i32(c, &i1, band_end, 1, NULL))
		return 0;
	if (hv < 2 || hsize <= 0 || hsize > HUFF_MAX || i0 < 0 || i1 <= i0 ||
		i0 >= hsize * 2 || (i1 - 1) >= hsize * 2 ||
		huff_wrap(i0, hsize) >= hsize || huff_wrap(i1 - 1, hsize) >= hsize) {
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		*withhold = 1;
		return drain(c, band_end);
	}
	nlen = i1 - i0;
	if (nlen <= 0 || nlen > hsize) {
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		*withhold = 1;
		return drain(c, band_end);
	}
	lengths = calloc((size_t)nlen, sizeof(unsigned int));
	clen = calloc((size_t)hsize, sizeof(unsigned short));
	ccode = calloc((size_t)hsize, sizeof(unsigned int));
	if (!lengths || !clen || !ccode) {
		free(lengths);
		free(clen);
		free(ccode);
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		*withhold = 1;
		return drain(c, band_end);
	}
	rc = bitstuff_pull(c, band_end, lengths, (size_t)nlen, &n_out);
	if (rc != 1 || n_out != (size_t)nlen) {
		free(lengths);
		free(clen);
		free(ccode);
		if (rc == -1)
			return 0;
		if (rc == -2)
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		else if (rc == 1)
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		*withhold = 1;
		return drain(c, band_end);
	}
	max_len = 0;
	sum = 0;
	for (i = 0; i < nlen; i++) {
		int len = (int)lengths[i];
		int idx = huff_wrap(i0 + i, hsize);
		if (len < 0 || len > 32) {
			free(lengths);
			free(clen);
			free(ccode);
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			*withhold = 1;
			return drain(c, band_end);
		}
		clen[idx] = (unsigned short)len;
		if (len > max_len)
			max_len = len;
		sum += (size_t)len;
	}
	free(lengths);
	lengths = NULL;
	if (max_len <= 0) {
		free(clen);
		free(ccode);
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		*withhold = 1;
		return drain(c, band_end);
	}
	code_bytes = sum == 0 ? 0 : (((sum + 7) >> 3) + 3) >> 2;
	code_bytes *= 4;
	coded = malloc(code_bytes ? code_bytes : 1);
	if (!coded || (code_bytes && !read_capped(c, coded, code_bytes, band_end, NULL))) {
		free(coded);
		free(clen);
		free(ccode);
		if (!coded) {
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
			*withhold = 1;
			return drain(c, band_end);
		}
		return 0;
	}
	off = 0;
	bit_pos = 0;
	for (i = 0; i < nlen; i++) {
		int idx = huff_wrap(i0 + i, hsize);
		int len = clen[idx];
		unsigned int code = 0;
		if (len <= 0)
			continue;
		if (!huff_peek(coded, code_bytes, off, bit_pos, len, &code)) {
			free(coded);
			free(clen);
			free(ccode);
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			*withhold = 1;
			return drain(c, band_end);
		}
		huff_advance(&off, &bit_pos, len);
		if (len < 32 && (code >> (unsigned)len) != 0) {
			free(coded);
			free(clen);
			free(ccode);
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			*withhold = 1;
			return drain(c, band_end);
		}
		ccode[idx] = code;
	}
	free(coded);
	if (off + (bit_pos > 0 ? 4u : 0u) != code_bytes) {
		free(clen);
		free(ccode);
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
		*withhold = 1;
		return drain(c, band_end);
	}
	num_bits = max_len < HUFF_LUT ? max_len : HUFF_LUT;
	lut_n = (size_t)1 << (unsigned)num_bits;
	lut = malloc(lut_n * sizeof(*lut));
	if (!lut) {
		free(clen);
		free(ccode);
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		*withhold = 1;
		return drain(c, band_end);
	}
	for (i = 0; i < (int)lut_n; i++) {
		lut[i].len = -1;
		lut[i].sym = -1;
	}
	for (k = 0; k < hsize; k++) {
		int len = clen[k];
		unsigned int code, nent, j;
		if (len <= 0 || len > num_bits)
			continue;
		code = ccode[k] << (unsigned)(num_bits - len);
		nent = 1u << (unsigned)(num_bits - len);
		if ((size_t)code >= lut_n || (size_t)code + nent > lut_n) {
			free(lut);
			free(clen);
			free(ccode);
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			*withhold = 1;
			return drain(c, band_end);
		}
		for (j = 0; j < nent; j++) {
			lut[code | j].len = (short)len;
			lut[code | j].sym = (short)k;
		}
	}
	if (c->pos > band_end) {
		free(lut);
		free(clen);
		free(ccode);
		*withhold = 1;
		return 0;
	}
	pay_n = (size_t)(band_end - c->pos);
	pay_at = c->pos;
	pay = malloc(pay_n ? pay_n : 1);
	img = calloc(1, sample_bytes ? sample_bytes : 1);
	if (!pay || !img || (pay_n && !read_capped(c, pay, pay_n, band_end, NULL))) {
		free(pay);
		free(img);
		free(lut);
		free(clen);
		free(ccode);
		if (!pay || !img) {
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
			*withhold = 1;
			return drain(c, band_end);
		}
		return 0;
	}
	off = 0;
	bit_pos = 0;
	stride = (size_t)cols * (size_t)depth;
	for (d = 0; d < (mode == 1 ? depth : 1); d++) {
		unsigned char prev = 0;
		int d0 = mode == 1 ? d : 0;
		int d1 = mode == 1 ? d + 1 : depth;
		for (row = 0; row < rows; row++) {
			for (col = 0; col < cols; col++) {
				size_t pix = (size_t)row * (size_t)cols + (size_t)col;
				int dd;
				if (bits && !bit_on(bits, pix))
					continue;
				for (dd = d0; dd < d1; dd++) {
					unsigned int window = 0;
					int sym = -1;
					int len;
					m = (pix * (size_t)depth) + (size_t)dd;
					if (!huff_peek(pay, pay_n, off, bit_pos, num_bits, &window) || (size_t)window >= lut_n) {
						sym = -1;
					} else if (lut[window].len >= 0) {
						sym = lut[window].sym;
						huff_advance(&off, &bit_pos, lut[window].len);
					} else {
						for (len = num_bits + 1; len <= max_len; len++) {
							unsigned int bits_n = 0;
							int s;
							if (!huff_peek(pay, pay_n, off, bit_pos, len, &bits_n))
								break;
							for (s = 0; s < hsize; s++) {
								if (clen[s] == len && ccode[s] == bits_n) {
									sym = s;
									huff_advance(&off, &bit_pos, len);
									break;
								}
							}
							if (sym >= 0)
								break;
						}
					}
					if (sym < 0) {
						free(pay);
						free(img);
						free(lut);
						free(clen);
						free(ccode);
						add_finding(c, pay_at + off, 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
						*withhold = 1;
						return 1;
					}
					{
						unsigned char delta = (unsigned char)(sym - (dt == 0 ? 128 : 0));
						unsigned char base = prev;
						int left_ok = col > 0 && (!bits || bit_on(bits, pix - 1));
						int above_ok = row > 0 && (!bits || bit_on(bits, pix - (size_t)cols));
						if (mode == 1) {
							if (left_ok)
								base = prev;
							else if (above_ok)
								base = img[m - stride];
							else
								base = prev;
							delta = (unsigned char)(delta + base);
							prev = delta;
						}
						img[m] = delta;
					}
				}
			}
		}
	}
	free(lut);
	free(clen);
	free(ccode);
	{
		size_t used = off + (size_t)((bit_pos > 0 ? 1 : 0) + 1) * 4u;
		if (used != pay_n) {
			free(pay);
			free(img);
			add_finding(c, pay_at + (used < pay_n ? used : pay_n), 1, 1, LERC_STREAM_CODE_CONSUMPTION, LERC_STREAM_CAT_FORMAT);
			*withhold = 1;
			return 1;
		}
	}
	free(pay);
	if (c->publishing && (c->report->options & LERC_STREAM_DISCARD) == 0)
		*samples = img;
	else
		free(img);
	return 1;
}

/* Flag byte 0. Byte and char Huffman are decoded here. Any other nonzero
 * mode stays unsupported. Returns 0 only when the walk must stop.
 * *withhold is set when this band must not be published.
 */
static int decode_flag0(Cur *c, uint64_t band_end, int version, int rows, int cols, int depth, int dt, int micro,
	double zerr, double zmax, const double *dmax, const unsigned char *bits, unsigned char **samples,
	size_t sample_bytes, int *withhold)
{
	int huff = (version >= 2 && (dt == 0 || dt == 1) && zerr == 0.5) ||
		(version >= 6 && (dt == 6 || dt == 7) && zerr == 0.0);
	int rc;
	unsigned char *img = NULL;
	if (huff) {
		unsigned char mode = 0;
		uint64_t at = 0;
		if (!read_capped(c, &mode, 1, band_end, &at))
			return 0;
		if (mode != 0) {
			if ((dt == 0 || dt == 1) && (mode == 1 || (mode == 2 && version >= 4)))
				return huffman_pull(c, band_end, rows, cols, depth, dt, mode, bits, samples,
					sample_bytes, withhold);
			add_finding(c, at, 1, 1, LERC_STREAM_CODE_UNSUPPORTED, LERC_STREAM_CAT_UNSUPPORTED);
			*withhold = 1;
			return drain(c, band_end);
		}
	}
	if (((c->report->options & LERC_STREAM_DISCARD) == 0 && c->publishing) || (version >= 5 && depth > 1)) {
		img = calloc(1, sample_bytes ? sample_bytes : 1);
		if (!img) {
			add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
			*withhold = 1;
			return drain(c, band_end);
		}
		*samples = img;
	}
	rc = read_tiles(c, band_end, version, rows, cols, depth, dt, micro, zerr, zmax, dmax, bits, img);
	if (rc == -1)
		return 0;
	if (rc == -2) {
		add_finding(c, c->pos, 0, 0, LERC_STREAM_CODE_BUFFER, LERC_STREAM_CAT_LIMIT);
		*withhold = 1;
		return drain(c, band_end);
	}
	if (rc == 0) {
		*withhold = 1;
		return drain(c, band_end);
	}
	return 1;
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
	double zmin = 0, zmax = 0, zerr = 0;
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
		if (i == 0)
			zerr = v;
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
						if (!decode_flag0(c, band_end, version, rows, cols, depth, dt, micro,
							zerr, zmax, dmax, bits, &samples, sample_bytes, &withhold)) {
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
