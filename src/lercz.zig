//! Thin Zig wrapper around the Esri LERC C ABI.
//!
//! Exposes the entire `Lerc_c_api.h` surface as `lercz.c.*`, matching
//! `pmarreck/zstdz`'s pattern for C-library wraps. Consumers call the C
//! functions directly; the wrapper adds no policy — that lives in the
//! caller (e.g. `tiffz` maps LERC errors to its `Malformed` set).
//!
//! LERC (Limited Error Raster Compression) is Esri's raster codec used by
//! GDAL and libtiff. TIFF integration: Compression tag 34887, with an
//! optional `LercParameters` tag (50674) carrying [codec_version,
//! add_compression] where add_compression ∈ {0 none, 1 Deflate, 2 Zstd}.

pub const c = @cImport({
	@cDefine("LERC_STATIC", "");
	@cInclude("Lerc_c_api.h");
	// Lerc_types.h is C++-only (uses namespaces) — reachable in comments
	// from Lerc_c_api.h but never included. The enum values are stable
	// and re-exported below as Zig constants.
});

/// Data type constants from Lerc_types.h::DataType (C++ namespace).
/// Values are stable per the Esri LERC public contract.
pub const dt_char: c_uint = 0;
pub const dt_uchar: c_uint = 1;
pub const dt_short: c_uint = 2;
pub const dt_ushort: c_uint = 3;
pub const dt_int: c_uint = 4;
pub const dt_uint: c_uint = 5;
pub const dt_float: c_uint = 6;
pub const dt_double: c_uint = 7;

/// Error code constants from Lerc_types.h::ErrCode. `Ok = 0`; any
/// nonzero return from a `c.lerc_*` call is a failure.
pub const err_ok: c_uint = 0;
pub const err_failed: c_uint = 1;
pub const err_wrong_param: c_uint = 2;
pub const err_buffer_too_small: c_uint = 3;
pub const err_nan: c_uint = 4;
pub const err_has_no_data: c_uint = 5;
pub const err_dimensions_too_large: c_uint = 6;

// ---- Smoke test ----

const std = @import("std");

test "lercz: exported dtype constants match Lerc_types.h" {
	// Guard against accidental drift if we ever re-derive these.
	try std.testing.expectEqual(@as(c_uint, 0), dt_char);
	try std.testing.expectEqual(@as(c_uint, 7), dt_double);
	try std.testing.expectEqual(@as(c_uint, 0), err_ok);
}

test "lercz.c: computeCompressedSizeForVersion is callable" {
	// Minimal 2x2 uint8 blob: exercise the C entry to prove linkage.
	const src = [_]u8{ 0, 1, 2, 3 };
	var out_size: u32 = 0;
	const status = c.lerc_computeCompressedSizeForVersion(
		&src[0],
		6, // codec version 6 (LERC2 latest per Lerc_c_api.h)
		dt_uchar,
		1, // nDepth
		2, // nCols
		2, // nRows
		1, // nBands
		0, // nMasks (0 = all valid)
		null, // pValidBytes
		0.0, // maxZErr
		&out_size,
	);
	try std.testing.expectEqual(@as(c_uint, err_ok), status);
	try std.testing.expect(out_size > 0);
}

test "lercz.c: uint8 lossless encode -> decode round-trip is byte-exact" {
	const allocator = std.testing.allocator;

	// 8x8 uint8 with a small ramp so LERC has real content to compress.
	var src: [64]u8 = undefined;
	for (&src, 0..) |*p, i| p.* = @intCast((i * 3) & 0xFF);

	var blob_size: u32 = 0;
	try std.testing.expectEqual(@as(c_uint, err_ok), c.lerc_computeCompressedSizeForVersion(
		&src[0], 6, dt_uchar, 1, 8, 8, 1, 0, null, 0.0, &blob_size,
	));
	try std.testing.expect(blob_size > 0);

	const blob = try allocator.alloc(u8, blob_size);
	defer allocator.free(blob);

	var written: u32 = 0;
	try std.testing.expectEqual(@as(c_uint, err_ok), c.lerc_encodeForVersion(
		&src[0], 6, dt_uchar, 1, 8, 8, 1, 0, null, 0.0,
		blob.ptr, blob_size, &written,
	));
	try std.testing.expect(written <= blob_size);
	try std.testing.expect(written > 0);

	var decoded: [64]u8 = undefined;
	try std.testing.expectEqual(@as(c_uint, err_ok), c.lerc_decode(
		blob.ptr, written, 0, null, 1, 8, 8, 1, dt_uchar, &decoded[0],
	));
	try std.testing.expectEqualSlices(u8, &src, &decoded);
}

test "lercz.c: reject oversized decode dimensions before reading data" {
	// LERC 4.2 limits uncompressed data per band to INT_MAX bytes. These
	// requests exceed that limit through pixel count, depth, or element size.
	const cases = [_]struct { dtype: c_uint, depth: c_int, cols: c_int, rows: c_int }{
		.{ .dtype = dt_uchar, .depth = 1, .cols = 65536, .rows = 32768 },
		.{ .dtype = dt_uchar, .depth = 2, .cols = 32768, .rows = 32768 },
		.{ .dtype = dt_float, .depth = 1, .cols = 32768, .rows = 16384 },
		.{ .dtype = dt_double, .depth = 1, .cols = 16384, .rows = 16384 },
	};
	const blob = [_]u8{0};
	var output: f64 = 123;
	for (cases) |case| {
		const status = c.lerc_decode(
			&blob, blob.len, 0, null, case.depth, case.cols, case.rows, 1, case.dtype, &output,
		);
		try std.testing.expectEqual(err_dimensions_too_large, status);
		try std.testing.expectEqual(@as(f64, 123), output);
	}
}
