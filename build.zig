const std = @import("std");

pub fn build(b: *std.Build) void {
	const target = b.standardTargetOptions(.{});
	const optimize = b.option(
		std.builtin.OptimizeMode,
		"optimize",
		"Optimization mode (default: ReleaseFast)",
	) orelse .ReleaseFast;
	const linkage = b.option(
		std.builtin.LinkMode,
		"linkage",
		"Linkage type for the library",
	) orelse .static;

	const cxx_flags = [_][]const u8{
		"-std=c++17",
	};

	const lerc_lib = b.addLibrary(.{
		.name = "lerc",
		.root_module = b.createModule(.{
			.target = target,
			.optimize = optimize,
			.link_libc = true,
			.link_libcpp = true,
		}),
		.linkage = linkage,
	});

	if (linkage == .static) {
		lerc_lib.root_module.addCMacro("LERC_STATIC", "");
	}

	lerc_lib.root_module.addIncludePath(b.path("src/LercLib"));
	lerc_lib.root_module.addIncludePath(b.path("src/LercLib/include"));
	lerc_lib.root_module.addIncludePath(b.path("src/LercLib/Lerc1Decode"));

	lerc_lib.root_module.addCSourceFiles(.{
		.root = b.path("src/LercLib"),
		.files = &top_sources,
		.flags = &cxx_flags,
		.language = .cpp,
	});
	lerc_lib.root_module.addCSourceFiles(.{
		.root = b.path("src/LercLib/Lerc1Decode"),
		.files = &lerc1_sources,
		.flags = &cxx_flags,
		.language = .cpp,
	});

	lerc_lib.installHeader(b.path("src/LercLib/include/Lerc_c_api.h"), "Lerc_c_api.h");
	lerc_lib.installHeader(b.path("src/LercLib/include/Lerc_types.h"), "Lerc_types.h");
	b.installArtifact(lerc_lib);

	const cli = b.addExecutable(.{
		.name = "lercz",
		.root_module = b.createModule(.{
			.target = target,
			.optimize = optimize,
			.link_libc = true,
			.link_libcpp = true,
		}),
	});
	cli.root_module.addCSourceFiles(.{ .files = &.{"src/cli.c"}, .flags = &.{ "-std=c11", "-Wall", "-Wextra", "-Werror" } });
	cli.root_module.addIncludePath(b.path("src/LercLib/include"));
	cli.root_module.linkLibrary(lerc_lib);
	if (linkage == .static) cli.root_module.addCMacro("LERC_STATIC", "");
	cli.root_module.addCMacro("LERCZ_DEBUG", if (optimize == .Debug) "1" else "0");
	cli.root_module.addCMacro("LERCZ_TARGET", b.fmt("\"{s}/{s}\"", .{ @tagName(target.result.os.tag), @tagName(target.result.cpu.arch) }));
	if (target.result.os.tag == .windows) cli.root_module.linkSystemLibrary("shell32", .{});
	b.installArtifact(cli);
	const cli_step = b.step("cli", "Build and install the C API dogfooding CLI");
	cli_step.dependOn(&b.addInstallArtifact(cli, .{}).step);

	const lerc_module = b.addModule("lercz", .{
		.root_source_file = b.path("src/lercz.zig"),
	});
	lerc_module.linkLibrary(lerc_lib);
	lerc_module.addIncludePath(b.path("src/LercLib/include"));
	if (linkage == .static) {
		lerc_module.addCMacro("LERC_STATIC", "");
	}

	const smoke = b.addTest(.{
		.root_module = b.createModule(.{
			.root_source_file = b.path("src/lercz.zig"),
			.target = target,
			.optimize = optimize,
			.link_libc = true,
			.link_libcpp = true,
		}),
	});
	smoke.root_module.addIncludePath(b.path("src/LercLib/include"));
	smoke.root_module.linkLibrary(lerc_lib);
	if (linkage == .static) {
		smoke.root_module.addCMacro("LERC_STATIC", "");
	}

	const run_smoke = b.addRunArtifact(smoke);
	const test_step = b.step("test", "Run lercz Zig-side smoke tests");
	test_step.dependOn(&run_smoke.step);
	const reference = b.addExecutable(.{
		.name = "lercz-c-reference",
		.root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = true }),
	});
	reference.root_module.addCSourceFiles(.{ .files = &.{"tests/ffi/reference.c"}, .flags = &.{"-std=c11"} });
	reference.root_module.addIncludePath(b.path("src/LercLib/include"));
	reference.root_module.linkLibrary(lerc_lib);
	if (linkage == .static) reference.root_module.addCMacro("LERC_STATIC", "");
	const cli_tests = b.addSystemCommand(&.{"bash"});
	cli_tests.addFileArg(b.path("tests/cli/codec"));
	cli_tests.addArtifactArg(cli);
	cli_tests.addArtifactArg(reference);
	test_step.dependOn(&cli_tests.step);
}

const top_sources = [_][]const u8{
	"BitMask.cpp",
	"BitStuffer2.cpp",
	"Huffman.cpp",
	"Lerc.cpp",
	"Lerc2.cpp",
	"Lerc_c_api_impl.cpp",
	"RLE.cpp",
	"fpl_Compression.cpp",
	"fpl_EsriHuffman.cpp",
	"fpl_Lerc2Ext.cpp",
	"fpl_Predictor.cpp",
	"fpl_UnitTypes.cpp",
};

const lerc1_sources = [_][]const u8{
	"BitStuffer.cpp",
	"CntZImage.cpp",
};
