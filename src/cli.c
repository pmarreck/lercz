/* Command-line adapter. The codec is accessed exclusively through its public C ABI. */
#define _POSIX_C_SOURCE 200809L
#include "Lerc_c_api.h"
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#ifndef LERCZ_TARGET
#define LERCZ_TARGET "unknown"
#endif
#ifndef LERCZ_DEBUG
#define LERCZ_DEBUG 0
#endif

static const char *types[] = {"i8", "u8", "i16", "u16", "i32", "u32", "f32", "f64"};
static const size_t widths[] = {1, 1, 2, 2, 4, 4, 4, 8};
static const char help[] =
	"lercz - encode and decode numerical raster tiles through the LERC C API\n"
	"Usage: lercz [options] encode|decode|check|info [INPUT]\n"
	"  encode  Raw little-endian samples (or a JSON number array) to a LERC blob\n"
	"  decode  LERC blob to raw little-endian samples (or JSON with --json)\n"
	"  check   Fully decode and discard samples; success is not an authenticity claim\n"
	"  info    Inspect blob metadata; this does not perform a full decode\n"
	"Input defaults to stdin; - and @stdin name stdin. -- ends option parsing.\n"
	"  -i, --input PATH        Input file, - or @stdin\n"
	"  -o, --output PATH       Output file, - or @stdout (default stdout)\n"
	"  --type TYPE            encode: i8,u8,i16,u16,i32,u32,f32,f64 (required)\n"
	"  --width N --height N   encode: pixel dimensions (required)\n"
	"  --depth N --bands N    encode: values per pixel and bands (default 1)\n"
	"  --error N              encode: maximum absolute error (default 0, lossless)\n"
	"  --mask PATH            encode: input validity bytes; decode: output validity bytes\n"
	"  --nodata N             encode: no-data sentinel for every band\n"
	"  --json-input           encode: read a flat JSON number array instead of raw bytes\n"
	"  --json                 info/check/decode: emit JSON\n"
	"  --json-errors          Emit errors as JSON on stderr\n"
	"  --json-metadata        Emit completion metadata as JSON on stderr\n"
	"  --max-bytes N          Limit each input/decoded allocation (default 268435456)\n"
	"  --progress, --no-progress  Completion byte counts on stderr (default TTY only)\n"
	"  --simple, --ascii      Plain ASCII presentation without color\n"
	"  --no-color, --no-ansi  Disable color (also honors NO_COLOR)\n"
	"  -h, --help, /?         This help\n"
	"  --about, --version     One-line version and compile target\n"
	"Samples are row-major, depth interleaved, bands sequential. Mask size is\n"
	"width*height (shared) or width*height*bands. Decode returns one mask per band.\n"
	"Invalid pixel values are unspecified; preserve the mask/no-data metadata.\n"
	"Exit: 0 success; 1 rejected blob; 2 usage/I/O; 3 resource/codec-size limit.\n";

typedef struct {
	const char *verb, *input, *output, *mask;
	unsigned type, width, height, depth, bands;
	double error, nodata;
	size_t limit;
	int have_type, have_nodata, json, json_input, json_errors, json_metadata;
	int progress, color, ascii, help, about;
} Options;
typedef struct {
	unsigned char *data;
	size_t size;
} Buffer;
static Options opt;
static const char *error_category, *error_message;
static unsigned codec_status;
static int have_codec_status;
static const char *error_code, *stage = "arguments";
static size_t input_bytes, error_offset, error_length;
static int offset_known;

static int failure(int status, const char *category, const char *message) {
	error_category = category;
	error_message = message;
	error_code = !strcmp(category, "io")	  ? "IoError"
				 : !strcmp(category, "usage") ? "InvalidArgument"
				 : !strcmp(category, "limit") ? "ResourceLimit"
											  : "InvalidBlobHeader";
	return status;
}
static int codec_failure(unsigned status, int encoding) {
	static const char *names[] = {
		"LercOk",  "LercFailed",	"LercWrongParam",		 "LercBufferTooSmall",
		"LercNaN", "LercHasNoData", "LercDimensionsTooLarge"};
	codec_status = status;
	have_codec_status = 1;
	int limit = status == 3 || status == 6;
	int rc = failure(limit		? 3
					 : encoding ? 2
								: 1,
					 limit ? "limit" : "codec", "LERC operation failed");
	error_code = status < 7 ? names[status] : "LercUnknownStatus";
	return rc;
}
static int is_input_stream(const char *s) { return !strcmp(s, "-") || !strcmp(s, "@stdin"); }
static int is_output_stream(const char *s) { return !strcmp(s, "-") || !strcmp(s, "@stdout"); }
#ifdef _WIN32
static wchar_t *wide(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if (!n)
		return NULL;
	wchar_t *w = malloc((size_t)n * sizeof(*w));
	if (w && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
		free(w);
		return NULL;
	}
	return w;
}
#endif
static FILE *open_file(const char *s, int writing) {
	if (writing ? is_output_stream(s) : is_input_stream(s))
		return writing ? stdout : stdin;
#ifdef _WIN32
	wchar_t *w = wide(s);
	if (!w)
		return NULL;
	FILE *f = _wfopen(w, writing ? L"wb" : L"rb");
	free(w);
	return f;
#else
	return fopen(s, writing ? "wb" : "rb");
#endif
}
static int read_all(const char *path, Buffer *b) {
	FILE *f = open_file(path, 0);
	if (!f)
		return failure(2, "io", "cannot open input");
	size_t cap = opt.limit < 65536 ? opt.limit : 65536;
	b->data = malloc(cap ? cap : 1);
	b->size = 0;
	int rc = 0;
	if (!b->data)
		rc = failure(3, "limit", "allocation failed");
	while (!rc) {
		if (b->size == cap) {
			int c = fgetc(f);
			if (c == EOF) {
				if (ferror(f))
					rc = failure(2, "io", "input read failed");
				break;
			}
			if (cap == opt.limit) {
				rc = failure(3, "limit", "input exceeds --max-bytes");
				break;
			}
			size_t next = cap > opt.limit / 2 ? opt.limit : cap * 2;
			unsigned char *p = realloc(b->data, next);
			if (!p) {
				rc = failure(3, "limit", "allocation failed");
				break;
			}
			b->data = p;
			cap = next;
			b->data[b->size++] = (unsigned char)c;
		}
		size_t n = fread(b->data + b->size, 1, cap - b->size, f);
		b->size += n;
		if (!n) {
			if (ferror(f))
				rc = failure(2, "io", "input read failed");
			break;
		}
	}
	if (f != stdin && fclose(f) && !rc)
		rc = failure(2, "io", "input close failed");
	return rc;
}
static int product(size_t a, size_t b, size_t *out) {
	if (b && a > opt.limit / b)
		return failure(3, "limit", "decoded size exceeds --max-bytes");
	*out = a * b;
	return 0;
}
static int positive(const char *s, size_t max, size_t *value) {
	if (!*s)
		return 0;
	size_t n = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || n > (max - (unsigned)(*s - '0')) / 10)
			return 0;
		n = n * 10 + (unsigned)(*s - '0');
	}
	if (!n || n > max)
		return 0;
	*value = n;
	return 1;
}
static int number(const char *s, double *v) {
	char *end;
	errno = 0;
	*v = strtod(s, &end);
	/* strtod may set ERANGE for a representable subnormal. Finiteness rejects
	 * overflow; normal floating-point rounding also permits underflow to zero. */
	return s != end && !*end && isfinite(*v);
}
static int parse(int argc, char **argv) {
	opt.input = "-";
	opt.output = "-";
	opt.depth = opt.bands = 1;
	opt.limit = 268435456;
	opt.progress = -1;
	opt.color = getenv("NO_COLOR") == NULL;
	int ended = 0, positional = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *value = NULL;
		if (!ended && !strcmp(a, "--")) {
			ended = 1;
			continue;
		}
		if (!ended &&
			(!strcmp(a, "encode") || !strcmp(a, "decode") || !strcmp(a, "check") ||
			 !strcmp(a, "info")) &&
			!opt.verb) {
			opt.verb = a;
			continue;
		}
		if (!ended &&
			(!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "/?") || !strcmp(a, "/h"))) {
			opt.help = 1;
			continue;
		}
		if (!ended && (!strcmp(a, "--about") || !strcmp(a, "--version"))) {
			opt.about = 1;
			continue;
		}
		if (!ended && !strcmp(a, "--json")) {
			opt.json = 1;
			continue;
		}
		if (!ended && !strcmp(a, "--json-input")) {
			opt.json_input = 1;
			continue;
		}
		if (!ended && !strcmp(a, "--json-errors")) {
			opt.json_errors = 1;
			continue;
		}
		if (!ended && !strcmp(a, "--json-metadata")) {
			opt.json_metadata = 1;
			continue;
		}
		if (!ended && (!strcmp(a, "--ascii") || !strcmp(a, "--simple"))) {
			opt.ascii = 1;
			opt.color = 0;
			continue;
		}
		if (!ended && (!strcmp(a, "--no-color") || !strcmp(a, "--no-ansi"))) {
			opt.color = 0;
			continue;
		}
		if (!ended && !strcmp(a, "--progress")) {
			opt.progress = 1;
			continue;
		}
		if (!ended && !strcmp(a, "--no-progress")) {
			opt.progress = 0;
			continue;
		}
		if (!ended &&
			((!strcmp(a, "-o")) || !strcmp(a, "--output") || !strcmp(a, "/o") || !strcmp(a, "-i") ||
			 !strcmp(a, "--input") || !strcmp(a, "--mask") || !strcmp(a, "--type") ||
			 !strcmp(a, "--width") || !strcmp(a, "--height") || !strcmp(a, "--depth") ||
			 !strcmp(a, "--bands") || !strcmp(a, "--max-bytes") || !strcmp(a, "--error") ||
			 !strcmp(a, "--nodata"))) {
			if (++i == argc)
				return failure(2, "usage", "option requires a value");
			value = argv[i];
			if (!strcmp(a, "-o") || !strcmp(a, "--output") || !strcmp(a, "/o"))
				opt.output = value;
			else if (!strcmp(a, "-i") || !strcmp(a, "--input"))
				opt.input = value;
			else if (!strcmp(a, "--mask"))
				opt.mask = value;
			else if (!strcmp(a, "--type")) {
				unsigned j;
				for (j = 0; j < 8 && strcmp(value, types[j]); j++) {
				}
				if (j == 8)
					return failure(2, "usage", "unknown sample type");
				opt.type = j;
				opt.have_type = 1;
			} else if (!strcmp(a, "--error") || !strcmp(a, "--nodata")) {
				double n;
				if (!number(value, &n))
					return failure(2, "usage", "expected a finite number");
				if (!strcmp(a, "--error")) {
					if (n < 0)
						return failure(2, "usage", "error bound must be nonnegative");
					opt.error = n;
				} else {
					opt.nodata = n;
					opt.have_nodata = 1;
				}
			} else {
				size_t n;
				if (!positive(value, !strcmp(a, "--max-bytes") ? UINT_MAX : INT_MAX, &n))
					return failure(2, "usage", "expected a positive bounded integer");
				if (!strcmp(a, "--width"))
					opt.width = (unsigned)n;
				else if (!strcmp(a, "--height"))
					opt.height = (unsigned)n;
				else if (!strcmp(a, "--depth"))
					opt.depth = (unsigned)n;
				else if (!strcmp(a, "--bands"))
					opt.bands = (unsigned)n;
				else
					opt.limit = n;
			}
			continue;
		}
		if (!ended && a[0] == '-' && a[1])
			return failure(2, "usage", "unknown option");
		if (positional++)
			return failure(2, "usage", "only one input file is allowed");
		opt.input = a;
	}
	if (opt.help || opt.about)
		return 0;
	if (!opt.verb)
		return failure(2, "usage", "expected encode, decode, check, or info");
	int encoding = !strcmp(opt.verb, "encode");
	if (encoding && (!opt.width || !opt.height || !opt.have_type))
		return failure(2, "usage", "encode requires --width, --height, and --type");
	if (encoding && opt.json)
		return failure(2, "usage", "encode produces a binary LERC blob; use --json-metadata");
	if (!encoding && (opt.json_input || opt.have_type || opt.width || opt.height ||
					  opt.have_nodata || opt.depth != 1 || opt.bands != 1 || opt.error != 0))
		return failure(2, "usage", "encoding options require encode");
	if (opt.mask && strcmp(opt.verb, "encode") && strcmp(opt.verb, "decode"))
		return failure(2, "usage", "--mask requires encode or decode");
	if (opt.mask && encoding && is_input_stream(opt.mask) && is_input_stream(opt.input))
		return failure(2, "usage", "pixels and mask cannot share stdin");
	if (opt.mask && !encoding &&
		((!strcmp(opt.mask, opt.output)) ||
		 (is_output_stream(opt.mask) && is_output_stream(opt.output))))
		return failure(2, "usage", "pixels and mask need separate output destinations");
	return 0;
}
static void json_number(FILE *f, double x) {
	if (isfinite(x))
		fprintf(f, "%.17g", x);
	else
		fputs("null", f);
}
static void json_string(FILE *f, const char *s) {
	if (!s) {
		fputs("null", f);
		return;
	}
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			fputc('\\', f);
			fputc(c, f);
		} else if (c < 32)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}
static double sample(const void *p, unsigned type, size_t i) {
	switch (type) {
	case 0:
		return ((const int8_t *)p)[i];
	case 1:
		return ((const uint8_t *)p)[i];
	case 2:
		return ((const int16_t *)p)[i];
	case 3:
		return ((const uint16_t *)p)[i];
	case 4:
		return ((const int32_t *)p)[i];
	case 5:
		return ((const uint32_t *)p)[i];
	case 6:
		return ((const float *)p)[i];
	default:
		return ((const double *)p)[i];
	}
}
static int store_sample(void *p, unsigned type, size_t i, double x) {
	static const double lo[] = {-128, 0, -32768, 0, -2147483648.0, 0, -FLT_MAX, -DBL_MAX};
	static const double hi[] = {127,		  255,			32767,	 65535,
								2147483647.0, 4294967295.0, FLT_MAX, DBL_MAX};
	if (!isfinite(x) || x < lo[type] || x > hi[type] || (type < 6 && trunc(x) != x))
		return 0;
	switch (type) {
	case 0:
		((int8_t *)p)[i] = (int8_t)x;
		break;
	case 1:
		((uint8_t *)p)[i] = (uint8_t)x;
		break;
	case 2:
		((int16_t *)p)[i] = (int16_t)x;
		break;
	case 3:
		((uint16_t *)p)[i] = (uint16_t)x;
		break;
	case 4:
		((int32_t *)p)[i] = (int32_t)x;
		break;
	case 5:
		((uint32_t *)p)[i] = (uint32_t)x;
		break;
	case 6:
		((float *)p)[i] = (float)x;
		break;
	default:
		((double *)p)[i] = x;
	}
	return 1;
}
static int whitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
/* JSON number grammar, deliberately restricted to a flat numeric array. */
static int parse_values(Buffer *b, void *out, size_t count, unsigned type) {
	size_t i = 0, k = 0;
	while (i < b->size && whitespace(b->data[i]))
		i++;
	if (i == b->size || b->data[i++] != '[')
		return 0;
	for (;;) {
		while (i < b->size && whitespace(b->data[i]))
			i++;
		if (i == b->size || k == count)
			return 0;
		size_t start = i;
		if (b->data[i] == '-')
			i++;
		if (i == b->size)
			return 0;
		if (b->data[i] == '0')
			i++;
		else {
			if (b->data[i] < '1' || b->data[i] > '9')
				return 0;
			while (i < b->size && b->data[i] >= '0' && b->data[i] <= '9')
				i++;
		}
		if (i < b->size && b->data[i] == '.') {
			i++;
			size_t first = i;
			while (i < b->size && b->data[i] >= '0' && b->data[i] <= '9')
				i++;
			if (i == first)
				return 0;
		}
		if (i < b->size && (b->data[i] == 'e' || b->data[i] == 'E')) {
			i++;
			if (i < b->size && (b->data[i] == '+' || b->data[i] == '-'))
				i++;
			size_t first = i;
			while (i < b->size && b->data[i] >= '0' && b->data[i] <= '9')
				i++;
			if (i == first)
				return 0;
		}
		char text[128];
		size_t len = i - start;
		if (len >= sizeof(text))
			return 0;
		memcpy(text, b->data + start, len);
		text[len] = 0;
		double x;
		if (!number(text, &x) || !store_sample(out, type, k++, x))
			return 0;
		while (i < b->size && whitespace(b->data[i]))
			i++;
		if (i == b->size)
			return 0;
		if (b->data[i] == ']') {
			i++;
			break;
		}
		if (b->data[i++] != ',')
			return 0;
	}
	while (i < b->size && whitespace(b->data[i]))
		i++;
	return i == b->size && k == count;
}
static void endian_swap(void *data, size_t count, size_t width) {
	uint16_t one = 1;
	if (*(unsigned char *)&one || width == 1)
		return;
	unsigned char *p = data;
	for (size_t i = 0; i < count; i++, p += width)
		for (size_t j = 0; j < width / 2; j++) {
			unsigned char t = p[j];
			p[j] = p[width - 1 - j];
			p[width - 1 - j] = t;
		}
}
static void metadata(FILE *f, const unsigned *info, const double *range) {
	fprintf(f,
			"{\"type\":\"%s\",\"width\":%u,\"height\":%u,\"depth\":%u,\"bands\":%u,\"codec_"
			"version\":%u,\"blob_bytes\":%u,\"masks\":%u,\"nodata_bands\":%u,\"min\":",
			types[info[1]], info[3], info[4], info[2], info[5], info[0], info[7], info[8],
			info[10]);
	json_number(f, range[0]);
	fputs(",\"max\":", f);
	json_number(f, range[1]);
	fputs(",\"max_error\":", f);
	json_number(f, range[2]);
	fputc('}', f);
}
static int write_raw(const char *path, const void *data, size_t n) {
	FILE *f = open_file(path, 1);
	if (!f)
		return failure(2, "io", "cannot open output");
	int bad = fwrite(data, 1, n, f) != n;
	if (f == stdout) {
		if (fflush(f))
			bad = 1;
	} else if (fclose(f))
		bad = 1;
	return bad ? failure(2, "io", "output write failed") : 0;
}
static int execute(void) {
	Buffer input = {0}, mask = {0};
	void *pixels = NULL;
	unsigned char *encoded = NULL, *valid = NULL, *uses = NULL;
	double *nodata = NULL;
	stage = "read";
	int rc = read_all(opt.input, &input);
	unsigned status = 0;
	input_bytes = input.size;
	unsigned info[11] = {0};
	double range[3] = {0};
	size_t count = 0, bytes = 0, mask_bytes = 0;
	int encoding = !strcmp(opt.verb, "encode");
	if (rc)
		goto done;
	if (encoding) {
		stage = "encode";
		if ((rc = product(opt.width, opt.height, &mask_bytes)) ||
			(rc = product(mask_bytes, opt.depth, &count)) ||
			(rc = product(count, opt.bands, &count)) ||
			(rc = product(count, widths[opt.type], &bytes)))
			goto done;
		pixels = malloc(bytes);
		if (!pixels) {
			rc = failure(3, "limit", "allocation failed");
			goto done;
		}
		if (opt.json_input) {
			if (!parse_values(&input, pixels, count, opt.type)) {
				rc = failure(2, "usage", "invalid JSON sample array or sample count/range");
				goto done;
			}
		} else {
			if (input.size != bytes) {
				rc = failure(2, "usage", "raw input length does not match dimensions and type");
				goto done;
			}
			memcpy(pixels, input.data, bytes);
			endian_swap(pixels, count, widths[opt.type]);
		}
		int masks = 0;
		if (opt.mask) {
			if ((rc = read_all(opt.mask, &mask)))
				goto done;
			if (mask.size == mask_bytes)
				masks = 1;
			else if (mask_bytes && mask.size / mask_bytes == opt.bands &&
					 mask.size % mask_bytes == 0)
				masks = (int)opt.bands;
			else {
				rc = failure(2, "usage", "mask length does not match dimensions");
				goto done;
			}
			for (size_t i = 0; i < mask.size; i++)
				if (mask.data[i] > 1) {
					rc = failure(2, "usage", "mask bytes must be zero or one");
					goto done;
				}
		}
		if (opt.have_nodata) {
			size_t no_bytes;
			if ((rc = product(opt.bands, sizeof(double), &no_bytes)))
				goto done;
			uses = calloc(opt.bands, 1);
			nodata = calloc(opt.bands, sizeof(double));
			if (!uses || !nodata) {
				rc = failure(3, "limit", "allocation failed");
				goto done;
			}
			for (unsigned i = 0; i < opt.bands; i++) {
				uses[i] = 1;
				nodata[i] = opt.nodata;
			}
		}
		unsigned needed = 0, written = 0;
		status = lerc_computeCompressedSize_4D(pixels, opt.type, (int)opt.depth, (int)opt.width,
											   (int)opt.height, (int)opt.bands, masks, mask.data,
											   opt.error, &needed, uses, nodata);
		if (status) {
			rc = codec_failure(status, 1);
			goto done;
		}
		if (needed > opt.limit) {
			rc = failure(3, "limit", "compressed size exceeds --max-bytes");
			goto done;
		}
		encoded = malloc(needed ? needed : 1);
		if (!encoded) {
			rc = failure(3, "limit", "allocation failed");
			goto done;
		}
		status = lerc_encode_4D(pixels, opt.type, (int)opt.depth, (int)opt.width, (int)opt.height,
								(int)opt.bands, masks, mask.data, opt.error, encoded, needed,
								&written, uses, nodata);
		if (status) {
			rc = codec_failure(status, 1);
			goto done;
		}
		if ((rc = write_raw(opt.output, encoded, written)))
			goto done;
		if (opt.json_metadata) {
			status = lerc_getBlobInfo(encoded, written, info, range, 11, 3);
			if (status) {
				rc = codec_failure(status, 1);
				goto done;
			}
			metadata(stderr, info, range);
			fputc('\n', stderr);
		}
	} else {
		stage = "metadata";
		status = lerc_getBlobInfo(input.data, (unsigned)input.size, info, range, 11, 3);
		if (status) {
			rc = codec_failure(status, 0);
			goto done;
		}
		if (info[1] > 7 || !info[2] || !info[3] || !info[4] || !info[5] || info[2] > INT_MAX ||
			info[3] > INT_MAX || info[4] > INT_MAX || info[5] > INT_MAX) {
			rc = failure(1, "format", "invalid blob dimensions or type");
			goto done;
		}
		if (info[7] != input.size) {
			rc = failure(1, "format", "trailing bytes after LERC blob");
			if (info[7] < input.size) {
				error_code = "TrailingData";
				offset_known = 1;
				error_offset = info[7];
				error_length = input.size - info[7];
			}
			goto done;
		}
		if (strcmp(opt.verb, "info")) {
			stage = "decode";
			if ((rc = product(info[3], info[4], &mask_bytes)) ||
				(rc = product(mask_bytes, info[5], &mask_bytes)) ||
				(rc = product(mask_bytes, info[2], &count)) ||
				(rc = product(count, widths[info[1]], &bytes)))
				goto done;
			size_t no_bytes;
			if ((rc = product(info[5], sizeof(double), &no_bytes)))
				goto done;
			pixels = calloc(bytes, 1);
			valid = calloc(mask_bytes, 1);
			uses = calloc(info[5], 1);
			nodata = calloc(info[5], sizeof(double));
			if (!pixels || !valid || !uses || !nodata) {
				rc = failure(3, "limit", "allocation failed");
				goto done;
			}
			status = lerc_decode_4D(input.data, (unsigned)input.size, (int)info[5], valid,
									(int)info[2], (int)info[3], (int)info[4], (int)info[5], info[1],
									pixels, uses, nodata);
			if (status) {
				rc = codec_failure(status, 0);
				goto done;
			}
		}
		if (!strcmp(opt.verb, "decode") && !opt.json) {
			endian_swap(pixels, count, widths[info[1]]);
			if ((rc = write_raw(opt.output, pixels, bytes)))
				goto done;
		} else {
			FILE *f = open_file(opt.output, 1);
			if (!f) {
				rc = failure(2, "io", "cannot open output");
				goto done;
			}
			if (opt.json && !strcmp(opt.verb, "check"))
				fputs("{\"ok\":true,\"decoded\":true}\n", f);
			else if (opt.json && !strcmp(opt.verb, "decode")) {
				fputs("{\"info\":", f);
				metadata(f, info, range);
				fputs(",\"values\":[", f);
				for (size_t i = 0; i < count; i++) {
					if (i)
						fputc(',', f);
					json_number(f, sample(pixels, info[1], i));
				}
				fputs("],\"mask\":[", f);
				for (size_t i = 0; i < mask_bytes; i++) {
					if (i)
						fputc(',', f);
					fprintf(f, "%u", valid[i]);
				}
				fputs("],\"nodata\":[", f);
				for (unsigned i = 0; i < info[5]; i++) {
					if (i)
						fputc(',', f);
					if (uses[i])
						json_number(f, nodata[i]);
					else
						fputs("null", f);
				}
				fputs("]}\n", f);
			} else if (opt.json) {
				metadata(f, info, range);
				fputc('\n', f);
			} else if (!strcmp(opt.verb, "info"))
				fprintf(f,
						"Type: %s\nDimensions: %u x %u\nDepth: %u\nBands: %u\nBlob bytes: "
						"%u\nMaximum error: %.17g\n",
						types[info[1]], info[3], info[4], info[2], info[5], info[7], range[2]);
			int bad = ferror(f);
			if (f == stdout) {
				if (fflush(f))
					bad = 1;
			} else if (fclose(f))
				bad = 1;
			if (bad) {
				rc = failure(2, "io", "output write failed");
				goto done;
			}
		}
		if (opt.mask && (rc = write_raw(opt.mask, valid, mask_bytes)))
			goto done;
		if (opt.json_metadata) {
			metadata(stderr, info, range);
			fputc('\n', stderr);
		}
	}
	if (!opt.json_metadata && (opt.progress == 1 || (opt.progress < 0 && isatty(fileno(stderr))))) {
		char line[160];
		snprintf(line, sizeof(line), "%s: %zu input bytes, %zu sample bytes processed\n", opt.verb,
				 input.size, bytes);
		fputs(line, stderr);
	}
done:
	free(input.data);
	free(mask.data);
	free(pixels);
	free(encoded);
	free(valid);
	free(uses);
	free(nodata);
	return rc;
}
static int cli_main(int argc, char **argv) {
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#else
	signal(SIGPIPE, SIG_IGN);
#endif
	int rc = parse(argc, argv);
	if (LERCZ_DEBUG && !getenv("MUTE_DEBUG_STATUS"))
		fputs(opt.color && isatty(fileno(stderr)) ? "\033[33mDEBUG BUILD!\033[0m\n"
												  : "DEBUG BUILD!\n",
			  stderr);
	if (!rc && opt.help) {
		fputs(help, stdout);
		return ferror(stdout) ? 2 : 0;
	}
	if (!rc && opt.about) {
		printf("lercz %d.%d.%d: LERC raster codec C CLI (%s)\n", LERC_VERSION_MAJOR,
			   LERC_VERSION_MINOR, LERC_VERSION_PATCH, LERCZ_TARGET);
		return ferror(stdout) ? 2 : 0;
	}
	if (!rc)
		rc = execute();
	if (rc) {
		if (opt.json_errors) {
			fprintf(stderr,
					"{\"schema\":\"lercz.error.v1\",\"ok\":false,\"category\":\"%s\",\"code\":\"%"
					"s\",\"detail\":\"%s\",\"class\":",
					error_category, error_code, error_code);
			if (!strcmp(error_category, "codec") || !strcmp(error_category, "format"))
				json_string(stderr, "failure");
			else
				fputs("null", stderr);
			fputs(",\"message\":", stderr);
			json_string(stderr, error_message);
			fputs(",\"operation\":", stderr);
			json_string(stderr, opt.verb);
			fputs(",\"stage\":", stderr);
			json_string(stderr, stage);
			fputs(",\"input\":", stderr);
			json_string(stderr, opt.input);
			fprintf(stderr, ",\"input_bytes\":%zu,\"lerc_status\":", input_bytes);
			if (have_codec_status)
				fprintf(stderr, "%u", codec_status);
			else
				fputs("null", stderr);
			fputs(",\"offset\":", stderr);
			if (offset_known)
				fprintf(stderr, "%zu", error_offset);
			else
				fputs("null", stderr);
			fprintf(stderr, ",\"offset_is_exact\":%s,\"block_offset\":null,\"blob_offset\":",
					offset_known ? "true" : "false");
			fputs(!strcmp(stage, "metadata") || !strcmp(stage, "decode") ? "0" : "null", stderr);
			fputs(",\"length\":", stderr);
			if (offset_known)
				fprintf(stderr, "%zu", error_length);
			else
				fputs("null", stderr);
			fputs("}\n", stderr);
		} else
			fprintf(stderr, "lercz: %s: %s (LERC status %u)\n", error_category, error_message,
					codec_status);
	}
	return rc;
}
int main(int argc, char **argv) {
#ifdef _WIN32
	(void)argc;
	(void)argv;
	int n = 0;
	wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
	if (!w)
		return 2;
	char **utf8 = calloc((size_t)n + 1, sizeof(*utf8));
	if (!utf8) {
		LocalFree(w);
		return 3;
	}
	int valid = 1;
	for (int i = 0; i < n; i++) {
		int size =
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w[i], -1, NULL, 0, NULL, NULL);
		if (!size || !(utf8[i] = malloc((size_t)size)) ||
			!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w[i], -1, utf8[i], size, NULL,
								 NULL)) {
			valid = 0;
			break;
		}
	}
	int rc = valid ? cli_main(n, utf8) : 2;
	for (int i = 0; i < n; i++)
		free(utf8[i]);
	free(utf8);
	LocalFree(w);
	return rc;
#else
	return cli_main(argc, argv);
#endif
}
