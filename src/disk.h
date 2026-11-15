#pragma once

// lib64 by Zac Walker
//
// C64 disk image (.d64/.d71/.d81) and tape archive (.t64) parsing interface:
// image formats, directory entries and PRG extraction from caller-provided
// image bytes.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class disk_image_format
{
	unknown,
	d64,
	d71,
	d81,
};

struct disk_directory_entry
{
	std::string filename; // printable ASCII, for display
	std::string type;
	uint16_t blocks = 0;
	bool closed = false;
	bool locked = false;
	// The name as the directory stores it, in PETSCII without its $A0 padding,
	// which is what LOAD has to be given to find exactly this file.
	std::vector<uint8_t> raw_name;
	uint8_t raw_type = 0;
};

struct disk_directory
{
	disk_image_format format = disk_image_format::unknown;
	std::string name;
	std::string id;
	uint16_t free_blocks = 0;
	std::vector<disk_directory_entry> entries;
};

// A file in a .t64 tape archive.
struct tape_entry
{
	std::string filename;          // printable ASCII, for display
	std::vector<uint8_t> raw_name; // PETSCII, without its space or $A0 padding
	uint16_t start_address = 0;
	size_t offset = 0; // of the data in the archive
	size_t length = 0;
};

const char* disk_format_name(disk_image_format format);

// All of these take a caller-owned image buffer and require `size` readable
// bytes at `data`. The image format is identified from `size` alone, so only
// exactly-sized images are accepted and every read stays inside the buffer.
// The directory lists every entry a drive lists, DEL entries included.
bool read_disk_directory(const uint8_t* data, size_t size, disk_directory& result);

// Extract a file from a disk image by following its sector chain. The returned
// bytes are in PRG form (2-byte little-endian load address followed by the
// program data), ready to hand to machine::load_prg. When filename is empty or
// "*" the first program on the disk is used (matching C64 LOAD"*",8); otherwise
// CBM DOS pattern matching applies ('*' matches the rest of the name, '?' any
// single character) over the PETSCII bytes, with ASCII lower case taken as the
// upper case a C64 types. Returns false if no file matches.
bool read_disk_file(const uint8_t* data, size_t size, std::string_view filename, std::vector<uint8_t>& result);

// As read_disk_file, for a name exactly as the C64 sends it: PETSCII bytes,
// nothing folded, so the graphics characters a directory name can hold match
// only themselves.
bool read_disk_file_petscii(const uint8_t* data, size_t size, const uint8_t* name, size_t length,
                            std::vector<uint8_t>& result);

// As read_disk_file_petscii, for one entry's name exactly as the directory
// stores it: '*' and '?' are themselves rather than a pattern, so a name such as
// "*** INTRO ***" reads that entry rather than the first file the pattern would
// match. Of two entries stored under one name, the first.
bool read_disk_file_entry(const uint8_t* data, size_t size, const uint8_t* name, size_t length,
                          std::vector<uint8_t>& result);

// The files in a .t64 tape archive, identified by its "C64" signature. A file
// whose header claims more than the archive holds is cut to what it holds.
bool read_tape_directory(const uint8_t* data, size_t size, std::vector<tape_entry>& entries);

// A file from a .t64 archive in PRG form, chosen as read_disk_file chooses one.
bool read_tape_file(const uint8_t* data, size_t size, std::string_view filename, std::vector<uint8_t>& result);

// As read_tape_file, for a name as the archive stores it: PETSCII bytes, nothing
// folded, as read_disk_file_petscii takes one.
bool read_tape_file_petscii(const uint8_t* data, size_t size, const uint8_t* name, size_t length,
                            std::vector<uint8_t>& result);

// As read_disk_file_entry, for an entry of a .t64 archive: its name as the
// archive stores it, without the padding, '*' and '?' included.
bool read_tape_file_entry(const uint8_t* data, size_t size, const uint8_t* name, size_t length,
                          std::vector<uint8_t>& result);

// Convert `length` PETSCII bytes to printable ASCII, stopping at a NUL or the
// $A0 shifted-space padding CBM DOS writes, and trimming trailing spaces.
std::string petscii_to_ascii(const uint8_t* data, size_t length);
