/* Independent C ABI interop fixture: no CLI parsing or serialization helpers. */
#include "Lerc_c_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
	if (argc != 3)
		return 2;
	const double values[] = {-1.5, 0, 0.25, 65536};
	unsigned char blob[4096];
	unsigned size = 0;
	if (!strcmp(argv[1], "version-regression")) {
		const double data[] = {1, -9999, 3, 4, 5, 6, -9999, 8};
		const unsigned char uses[] = {1, 1};
		const double nodata[] = {-9999, -9999};
		double out[8], decoded_nodata[2];
		unsigned char decoded_uses[2], mask[4];
		if (lerc_encode_4D(data, 7, 2, 2, 1, 2, 0, NULL, 0, blob,
						   sizeof(blob), &size, uses, nodata))
			return 10;
		if (blob[6] != 6)
			return 11;
		/* corruption-reader master 42, round 414: unchecked version bit. */
		blob[6] ^= 2;
		for (int bands = 1; bands <= 2; bands++) {
			if (!lerc_decode_4D(blob, size, bands, mask, 2, 2, 1, bands, 7,
							out, decoded_uses, decoded_nodata))
				return 12;
		}
		return 0;
	}
	if (!strcmp(argv[1], "old-versions")) {
		for (int version = 2; version <= 6; version++) {
			for (int constant = 0; constant <= 1; constant++) {
				double data[8] = {1, 2, 3, 4, 5, 6, 7, 8}, out[8];
				if (constant)
					for (int i = 0; i < 8; i++) data[i] = 3;
				if (lerc_encodeForVersion(data, version, 7, 1, 2, 2, 2, 0,
						NULL, 0, blob, sizeof(blob), &size)) return 13;
				for (int bands = 1; bands <= 2; bands++) {
					if (lerc_decode(blob, size, 0, NULL, 1, 2, 2, bands, 7, out)) return 14;
					if (memcmp(data, out, bands * 4 * sizeof(double))) return 15;
				}
			}
		}
		return 0;
	}
	if (!strcmp(argv[1], "encode")) {
		if (lerc_encode(values, 7, 1, 2, 2, 1, 0, NULL, 0, blob, sizeof(blob), &size))
			return 3;
		FILE *f = fopen(argv[2], "wb");
		if (!f)
			return 4;
		int bad = fwrite(blob, 1, size, f) != size;
		return fclose(f) || bad;
	}
	FILE *f = fopen(argv[2], "rb");
	if (!f)
		return 4;
	size = fread(blob, 1, sizeof(blob), f);
	int bad = ferror(f);
	fclose(f);
	if (bad)
		return 5;
	double out[4] = {0};
	if (lerc_decode(blob, size, 0, NULL, 1, 2, 2, 1, 7, out))
		return 6;
	return memcmp(values, out, sizeof(values)) != 0;
}
