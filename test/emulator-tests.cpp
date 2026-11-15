// lib64 by Zac Walker
//
// Headless behavioural test suite: 6502 CPU, VIC-II, CIA and assembler tests,
// plus boot/BASIC/SID/disk checks and the command-line test runners.

#include "disk.h"
#include "machine.h"
#include "assembler.h"
#include "debug.h"
#include "opcodes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace
{
	constexpr int execution_step = 100'000;
	constexpr int boot_cycle_limit = 5'000'000;
	constexpr int command_cycle_limit = 2'000'000;

	uint8_t screen_code(const char c)
	{
		if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A' + 1);
		return static_cast<uint8_t>(c);
	}

	bool screen_contains(const machine& c64, const std::string_view text)
	{
		const auto* screen = c64._state->RAM + text_video_mem_offset;
		for (size_t offset = 0; offset + text.size() <= 1000; ++offset)
		{
			bool matches = true;
			for (size_t index = 0; index < text.size(); ++index)
			{
				if ((screen[offset + index] & 0x7f) != screen_code(text[index]))
				{
					matches = false;
					break;
				}
			}
			if (matches) return true;
		}
		return false;
	}

	bool run_until_screen_contains(machine& c64, const std::string_view text, const int cycle_limit)
	{
		for (auto cycles = 0; cycles <= cycle_limit; cycles += execution_step)
		{
			if (screen_contains(c64, text)) return true;
			c64.exec(execution_step);
		}
		return false;
	}

	// Drive the machine the way the GUI app's 50 Hz timer does: fire the periodic
	// interrupt and run a frame's worth of cycles, repeatedly. A correct 6502 IRQ
	// vectors through $FFFE ($FF48) so the kernal handler's register save/restore
	// stays balanced; if it does not, the handler's closing RTI returns to a
	// garbage address and the kernal wipes the screen.
	void run_app_style_ticks(machine& c64, const int ticks)
	{
		constexpr int cycles_per_frame = 1'000'000 / 50;
		for (auto tick = 0; tick < ticks; ++tick)
		{
			c64.irq();
			c64.exec(cycles_per_frame);
		}
	}

	void type_line(machine& c64, const std::string_view text)
	{
		for (const auto c : text)
		{
			while (c64._state->RAM[mem_length_of_keyboard_buffer] >= 10) c64.exec(20'000);
			c64.add_char(c);
		}
		while (c64._state->RAM[mem_length_of_keyboard_buffer] >= 10) c64.exec(20'000);
		c64.add_char(13);
	}

	std::vector<uint8_t> read_file(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream) return {};
		const auto size = stream.tellg();
		if (size <= 0) return {};
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), size);
		return bytes;
	}

	bool list_disk(const std::filesystem::path& path, disk_directory* parsed = nullptr)
	{
		const auto bytes = read_file(path);
		disk_directory directory;
		if (!read_disk_directory(bytes.data(), bytes.size(), directory))
		{
			std::cerr << "Unable to read disk directory: " << path.string() << '\n';
			return false;
		}

		std::cout << disk_format_name(directory.format) << " disk: \"" << directory.name << "\"";
		if (!directory.id.empty()) std::cout << " ID " << directory.id;
		std::cout << '\n';
		for (const auto& entry : directory.entries)
		{
			std::cout << "  " << entry.blocks << "  \"" << entry.filename << "\" " << entry.type;
			if (!entry.closed) std::cout << '*';
			if (entry.locked) std::cout << '<';
			std::cout << '\n';
		}
		std::cout << "  " << directory.free_blocks << " blocks free.\n";

		if (parsed) *parsed = std::move(directory);
		return true;
	}

	bool has_entry_starting_with(const disk_directory& directory, const std::string_view prefix)
	{
		return std::any_of(directory.entries.begin(), directory.entries.end(),
		                   [prefix](const auto& entry) { return entry.filename.starts_with(prefix); });
	}

	bool recognizes_empty_disk(const size_t image_size, const size_t header_offset,
	                           const disk_image_format expected_format)
	{
		std::vector<uint8_t> bytes(image_size);
		bytes[header_offset] = 0;
		disk_directory directory;
		return read_disk_directory(bytes.data(), bytes.size(), directory) && directory.format == expected_format;
	}

	// 10 PRINT "<text>" as a .prg loading at BASIC's start, $0801.
	std::vector<uint8_t> make_basic_print(const std::string_view text)
	{
		const auto next_line = static_cast<uint16_t>(0x0801 + 9 + text.size());
		// clang-format off
		std::vector<uint8_t> prg = {
			0x01, 0x08,
			static_cast<uint8_t>(next_line), static_cast<uint8_t>(next_line >> 8),
			10, 0, // line 10
			0x99, ' ', '"', // PRINT "
		};
		// clang-format on
		prg.insert(prg.end(), text.begin(), text.end());
		prg.insert(prg.end(), {'"', 0, 0, 0}); // the end of the line, then of the program
		return prg;
	}

	// A 35-track D64 whose directory's first sector lists the programs given, in
	// order, each stored in one sector of track 1.
	std::vector<uint8_t> make_d64(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
	{
		constexpr size_t sector_size = 256;
		constexpr size_t directory_track = 17 * 21 * sector_size; // after tracks 1-17, of 21 sectors each
		std::vector<uint8_t> disk(174848, 0);
		disk[directory_track] = 18; // the directory starts at 18/1
		disk[directory_track + 1] = 1;
		auto* const directory = disk.data() + directory_track + sector_size;
		directory[1] = 0xFF; // and ends there

		for (size_t i = 0; i < files.size() && i < 8; ++i)
		{
			const auto& [name, prg] = files[i];
			auto* const entry = directory + i * 32;
			entry[2] = 0x82; // a closed PRG
			entry[3] = 1;
			entry[4] = static_cast<uint8_t>(i);
			std::fill_n(entry + 5, 16, static_cast<uint8_t>(0xA0));
			std::copy_n(name.begin(), std::min<size_t>(name.size(), 16), entry + 5);
			entry[30] = 1; // one block

			auto* const block = disk.data() + i * sector_size;
			block[1] = static_cast<uint8_t>(std::min<size_t>(prg.size(), 254) + 1); // the last byte used
			std::copy_n(prg.begin(), std::min<size_t>(prg.size(), 254), block + 2);
		}
		return disk;
	}

	uint16_t read_big_endian_16(const std::vector<uint8_t>& bytes, const size_t offset)
	{
		return static_cast<uint16_t>(bytes[offset] << 8 | bytes[offset + 1]);
	}

	uint32_t read_big_endian_32(const std::vector<uint8_t>& bytes, const size_t offset)
	{
		return static_cast<uint32_t>(bytes[offset]) << 24 | static_cast<uint32_t>(bytes[offset + 1]) << 16 |
		       static_cast<uint32_t>(bytes[offset + 2]) << 8 | bytes[offset + 3];
	}

	struct crt_info
	{
		bool valid = false;
		uint16_t hardware_type = 0;
		size_t chip_count = 0;
		size_t rom_size = 0;
	};

	crt_info inspect_crt(const std::vector<uint8_t>& bytes)
	{
		constexpr std::string_view signature = "C64 CARTRIDGE   ";
		crt_info result;
		if (bytes.size() < 0x40 || !std::equal(signature.begin(), signature.end(), bytes.begin())) return result;

		const auto header_size = read_big_endian_32(bytes, 0x10);
		if (header_size < 0x40 || header_size > bytes.size()) return result;
		result.hardware_type = read_big_endian_16(bytes, 0x16);

		auto offset = static_cast<size_t>(header_size);
		constexpr std::string_view chip_signature = "CHIP";
		while (offset < bytes.size())
		{
			// One named object, not two literals: "CHIP" and "CHIP" + 4 are distinct
			// arrays unless the compiler pools them, so that range is not valid.
			if (bytes.size() - offset < 16 ||
			    !std::equal(chip_signature.begin(), chip_signature.end(), bytes.begin() + offset))
				return result;

			const auto packet_size = read_big_endian_32(bytes, offset + 4);
			const auto rom_size = read_big_endian_16(bytes, offset + 14);
			if (packet_size < 16 || rom_size == 0 || rom_size > 0x4000 || packet_size < 16u + rom_size ||
			    packet_size > bytes.size() - offset)
				return result;

			++result.chip_count;
			result.rom_size += rom_size;
			offset += packet_size;
		}

		result.valid = result.chip_count != 0;
		return result;
	}

	struct test_runner
	{
		int failures = 0;
		int checks = 0;

		void check(const bool result, const std::string_view name)
		{
			++checks;
			std::cout << (result ? "PASS " : "FAIL ") << name << '\n';
			if (!result) ++failures;
		}
	};

	// An I/O register or colour RAM cell, as a reference into the chip's own
	// storage, bypassing the bus and its side effects.
	uint8_t& io(machine& m, const uint16_t address)
	{
		auto* s = m._state;
		if (address >= 0xD800 && address < 0xDC00) return s->color_ram[address & 0x3FF];
		if (address >= 0xDC00 && address < 0xDD00) return s->cia1.regs[address & 0x0F];
		if (address >= 0xDD00 && address < 0xDE00) return s->cia2.regs[address & 0x0F];
		return s->vic.regs[address & 0x3F];
	}

	// ======================================================================
	// Headless 6502 CPU test harness
	// ======================================================================
	// Runs small hand-assembled programs on a bare 6502 + flat 64K RAM (no
	// banking, no I/O), the way Klaus Dormann's functional test expects. Each
	// program ends at a JMP-to-self trap so we can detect completion.
	struct cpu_test
	{
		machine m;

		cpu_test() { m._state->raw_ram = true; }

		void poke(uint16_t addr, std::initializer_list<uint8_t> bytes)
		{
			for (const uint8_t b : bytes) m._state->RAM[addr++] = b;
		}

		void poke(uint16_t addr, uint8_t value) { m._state->RAM[addr] = value; }

		// Load code at $0200, append a JMP-to-self trap, run until it traps.
		void run_prog(std::vector<uint8_t> code)
		{
			constexpr uint16_t base = 0x0200;
			const auto trap = static_cast<uint16_t>(base + code.size());
			code.push_back(0x4C);
			code.push_back(static_cast<uint8_t>(trap & 0xFF));
			code.push_back(static_cast<uint8_t>((trap >> 8) & 0xFF));
			uint16_t p = base;
			for (const uint8_t b : code) m._state->RAM[p++] = b;

			m._state->cpu.a = m._state->cpu.x = m._state->cpu.y = 0;
			m._state->cpu.sp = 0xFD;
			m._state->cpu.status = FLAG_CONSTANT;
			m._state->cpu.pc = base;
			for (int i = 0; i < 100000; ++i)
			{
				const uint16_t before = m._state->cpu.pc;
				m.exec(1);
				if (m._state->cpu.pc == before) break;
			}
		}

		// Cycles consumed by one instruction assembled at $0200.
		int64_t instr_cycles(std::initializer_list<uint8_t> code, uint8_t init_status = FLAG_CONSTANT)
		{
			uint16_t p = 0x0200;
			for (const uint8_t b : code) m._state->RAM[p++] = b;
			m._state->cpu.pc = 0x0200;
			m._state->cpu.status = init_status;
			const int64_t before = m._state->clock_ticks;
			m.exec(1);
			return m._state->clock_ticks - before;
		}

		uint8_t a() const { return m._state->cpu.a; }
		uint8_t x() const { return m._state->cpu.x; }
		uint8_t y() const { return m._state->cpu.y; }
		uint8_t mem(uint16_t addr) const { return m._state->RAM[addr]; }
		bool flag(uint8_t f) const { return (m._state->cpu.status & f) != 0; }
	};

	void run_cpu_tests(test_runner& tests)
	{
		// --- Loads / flags ---
		// clang-format off
		{ cpu_test t; t.run_prog({0xA9, 0x00}); tests.check(t.a() == 0x00 && t.flag(FLAG_ZERO) && !t.flag(FLAG_SIGN), "LDA #0 sets Z"); }
		{ cpu_test t; t.run_prog({0xA9, 0x80}); tests.check(t.a() == 0x80 && t.flag(FLAG_SIGN) && !t.flag(FLAG_ZERO), "LDA #$80 sets N"); }
		{ cpu_test t; t.run_prog({0xA9, 0x42, 0xAA}); tests.check(t.x() == 0x42, "TAX transfers A to X"); }

		// --- ADC / SBC (binary + decimal) ---
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0x50, 0x69, 0x50}); tests.check(t.a() == 0xA0 && t.flag(FLAG_OVERFLOW) && t.flag(FLAG_SIGN) && !t.flag(FLAG_CARRY), "ADC signed overflow"); }
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0xFF, 0x69, 0x01}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY) && t.flag(FLAG_ZERO), "ADC carry out"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x09, 0x69, 0x01}); tests.check(t.a() == 0x10, "ADC decimal 09+01=10"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY), "ADC decimal 99+01 carries"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x50, 0xE9, 0x10}); tests.check(t.a() == 0x40 && t.flag(FLAG_CARRY), "SBC borrowless"); }
		{ cpu_test t; t.run_prog({0x18, 0xA9, 0x50, 0xE9, 0x10}); tests.check(t.a() == 0x3F, "SBC with borrow"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x50, 0xE9, 0x25}); tests.check(t.a() == 0x25 && t.flag(FLAG_CARRY), "SBC decimal 50-25=25"); }

		// --- Decimal mode (BCD): value + carry are deterministic on the NMOS 6502
		//     (cases drawn from Bruce Clark's decimal-mode reference). ---
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x24, 0x69, 0x56}); tests.check(t.a() == 0x80 && !t.flag(FLAG_CARRY), "ADC decimal 24+56=80"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x93, 0x69, 0x82}); tests.check(t.a() == 0x75 && t.flag(FLAG_CARRY), "ADC decimal 93+82=175 carries"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x50, 0x69, 0x50}); tests.check(t.a() == 0x00 && t.flag(FLAG_CARRY), "ADC decimal 50+50=100 carries"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x00, 0xE9, 0x01}); tests.check(t.a() == 0x99 && !t.flag(FLAG_CARRY), "SBC decimal 00-01 borrows to 99"); }
		{ cpu_test t; t.run_prog({0xF8, 0x38, 0xA9, 0x46, 0xE9, 0x12}); tests.check(t.a() == 0x34 && t.flag(FLAG_CARRY), "SBC decimal 46-12=34"); }
		{ cpu_test t; t.run_prog({0xF8, 0x18, 0xA9, 0x32, 0xE9, 0x02}); tests.check(t.a() == 0x29, "SBC decimal 32-02 with borrow-in = 29"); }

		// --- Logic / shifts / rotates ---
		{ cpu_test t; t.run_prog({0xA9, 0xCC, 0x29, 0x0F}); tests.check(t.a() == 0x0C, "AND immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0xC0, 0x09, 0x0F}); tests.check(t.a() == 0xCF, "ORA immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0xFF, 0x49, 0x0F}); tests.check(t.a() == 0xF0, "EOR immediate"); }
		{ cpu_test t; t.run_prog({0xA9, 0x81, 0x0A}); tests.check(t.a() == 0x02 && t.flag(FLAG_CARRY), "ASL A"); }
		{ cpu_test t; t.run_prog({0xA9, 0x03, 0x4A}); tests.check(t.a() == 0x01 && t.flag(FLAG_CARRY), "LSR A"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x80, 0x2A}); tests.check(t.a() == 0x01 && t.flag(FLAG_CARRY), "ROL A through carry"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0x01, 0x6A}); tests.check(t.a() == 0x80 && t.flag(FLAG_CARRY) && t.flag(FLAG_SIGN), "ROR A through carry"); }

		// --- Inc / dec ---
		{ cpu_test t; t.run_prog({0xA2, 0xFF, 0xE8}); tests.check(t.x() == 0x00 && t.flag(FLAG_ZERO), "INX wraps to 0"); }
		{ cpu_test t; t.run_prog({0xA2, 0x00, 0xCA}); tests.check(t.x() == 0xFF && t.flag(FLAG_SIGN), "DEX wraps to $FF"); }
		{ cpu_test t; t.run_prog({0xA9, 0x7F, 0x8D, 0x00, 0x03, 0xEE, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x80, "INC memory"); }
		{ cpu_test t; t.run_prog({0xA9, 0x01, 0x8D, 0x00, 0x03, 0xCE, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x00, "DEC memory"); }

		// --- Addressing modes ---
		{ cpu_test t; t.run_prog({0xA2, 0x05, 0xA9, 0xAA, 0x95, 0x80}); tests.check(t.mem(0x85) == 0xAA, "STA zero-page,X"); }
		{ cpu_test t; t.poke(0x0313, 0x77); t.run_prog({0xA2, 0x03, 0xBD, 0x10, 0x03}); tests.check(t.a() == 0x77, "LDA absolute,X"); }
		{ cpu_test t; t.poke(0x0010, {0x00, 0x04}); t.poke(0x0402, 0x99); t.run_prog({0xA0, 0x02, 0xB1, 0x10}); tests.check(t.a() == 0x99, "LDA (indirect),Y"); }

		// --- Compares / bit ---
		{ cpu_test t; t.run_prog({0xA9, 0x50, 0xC9, 0x50}); tests.check(t.flag(FLAG_ZERO) && t.flag(FLAG_CARRY), "CMP equal"); }
		{ cpu_test t; t.run_prog({0xA9, 0x50, 0xC9, 0x40}); tests.check(t.flag(FLAG_CARRY) && !t.flag(FLAG_ZERO), "CMP greater"); }
		{ cpu_test t; t.run_prog({0xA9, 0x40, 0xC9, 0x50}); tests.check(!t.flag(FLAG_CARRY) && t.flag(FLAG_SIGN), "CMP less"); }
		{ cpu_test t; t.poke(0x0300, 0xC0); t.run_prog({0xA9, 0x00, 0x2C, 0x00, 0x03}); tests.check(t.flag(FLAG_SIGN) && t.flag(FLAG_OVERFLOW) && t.flag(FLAG_ZERO), "BIT sets N/V from memory"); }

		// --- Stack / subroutines / branches ---
		{ cpu_test t; t.run_prog({0xA9, 0x33, 0x48, 0xA9, 0x00, 0x68}); tests.check(t.a() == 0x33, "PHA/PLA round-trip"); }
		{ cpu_test t; t.poke(0x0220, {0xA9, 0x5A, 0x60}); t.run_prog({0x20, 0x20, 0x02, 0x8D, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x5A && t.a() == 0x5A, "JSR/RTS"); }
		{ cpu_test t; t.run_prog({0xA9, 0x00, 0xF0, 0x02, 0xA9, 0xFF}); tests.check(t.a() == 0x00, "BEQ taken skips code"); }
		{ cpu_test t; t.run_prog({0xA9, 0x00, 0xD0, 0x02, 0xA9, 0xFF}); tests.check(t.a() == 0xFF, "BNE not taken falls through"); }

		// --- Cycle counts (base timings + the 6502 page-cross penalty) ---
		{ cpu_test t; tests.check(t.instr_cycles({0xEA}) == 2, "cycles: NOP = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xA9, 0x00}) == 2, "cycles: LDA # = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xA5, 0x00}) == 3, "cycles: LDA zp = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xAD, 0x00, 0x02}) == 4, "cycles: LDA abs = 4"); }
		{ cpu_test t; t.m._state->cpu.x = 1; tests.check(t.instr_cycles({0xBD, 0x00, 0x02}) == 4, "cycles: LDA abs,X no page cross = 4"); }
		{ cpu_test t; t.m._state->cpu.x = 1; tests.check(t.instr_cycles({0xBD, 0xFF, 0x02}) == 5, "cycles: LDA abs,X page cross = 5"); }
		{ cpu_test t; t.m._state->cpu.y = 1; tests.check(t.instr_cycles({0xB9, 0xFF, 0x02}) == 5, "cycles: LDA abs,Y page cross = 5"); }
		{ cpu_test t; tests.check(t.instr_cycles({0x4C, 0x00, 0x02}) == 3, "cycles: JMP abs = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0x20, 0x00, 0x02}) == 6, "cycles: JSR = 6"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0x02}, FLAG_CONSTANT) == 2, "cycles: branch not taken = 2"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0x02}, FLAG_CONSTANT | FLAG_ZERO) == 3, "cycles: branch taken same page = 3"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xF0, 0xFD}, FLAG_CONSTANT | FLAG_ZERO) == 4, "cycles: branch taken page cross = 4"); }

		// --- Undocumented opcodes, as the NMOS 6510 runs them ---
		{ cpu_test t; t.poke(0x0300, 0x5A); t.run_prog({0xAF, 0x00, 0x03}); tests.check(t.a() == 0x5A && t.x() == 0x5A, "LAX loads A and X"); }
		{ cpu_test t; t.run_prog({0xA9, 0xF0, 0xA2, 0x3C, 0x8F, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x30, "SAX stores A AND X"); }
		{ cpu_test t; t.poke(0x0300, 0x41); t.run_prog({0xA9, 0x40, 0xCF, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x40 && t.flag(FLAG_ZERO) && t.flag(FLAG_CARRY), "DCP decrements then compares"); }
		{ cpu_test t; t.poke(0x0300, 0x0F); t.run_prog({0x38, 0xA9, 0x20, 0xEF, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x10 && t.a() == 0x10 && t.flag(FLAG_CARRY), "ISC increments then subtracts"); }
		{ cpu_test t; t.poke(0x0300, 0x81); t.run_prog({0xA9, 0x01, 0x0F, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x02 && t.a() == 0x03 && t.flag(FLAG_CARRY), "SLO shifts then ORs"); }
		{ cpu_test t; t.poke(0x0300, 0x81); t.run_prog({0x38, 0xA9, 0x0F, 0x2F, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x03 && t.a() == 0x03 && t.flag(FLAG_CARRY), "RLA rotates then ANDs"); }
		{ cpu_test t; t.poke(0x0300, 0x03); t.run_prog({0xA9, 0xFF, 0x4F, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x01 && t.a() == 0xFE && t.flag(FLAG_CARRY), "SRE shifts then EORs"); }
		{ cpu_test t; t.poke(0x0300, 0x03); t.run_prog({0x18, 0xA9, 0x10, 0x6F, 0x00, 0x03}); tests.check(t.mem(0x0300) == 0x01 && t.a() == 0x12, "RRA rotates then adds the carry it shifted out"); }
		{ cpu_test t; t.run_prog({0xA9, 0xF0, 0x0B, 0x80}); tests.check(t.a() == 0x80 && t.flag(FLAG_CARRY) && t.flag(FLAG_SIGN), "ANC copies N into C"); }
		{ cpu_test t; t.run_prog({0xA9, 0xFF, 0x4B, 0x03}); tests.check(t.a() == 0x01 && t.flag(FLAG_CARRY), "ALR ANDs then shifts right"); }
		{ cpu_test t; t.run_prog({0x38, 0xA9, 0xFF, 0x6B, 0xC0}); tests.check(t.a() == 0xE0 && t.flag(FLAG_CARRY) && !t.flag(FLAG_OVERFLOW), "ARR rotates and sets C from bit 6"); }
		{ cpu_test t; t.run_prog({0xA9, 0xF0, 0xA2, 0x3C, 0xCB, 0x10}); tests.check(t.x() == 0x20 && t.flag(FLAG_CARRY), "SBX subtracts from A AND X"); }
		{ cpu_test t; t.poke(0x0300, 0x0F); t.run_prog({0xA0, 0x00, 0xBB, 0x00, 0x03}); tests.check(t.a() == 0x0D && t.x() == 0x0D && t.m._state->cpu.sp == 0x0D, "LAS ANDs memory with SP into A, X and SP"); }
		{ cpu_test t; t.run_prog({0xA2, 0xFF, 0xA0, 0x01, 0x9E, 0x00, 0x03}); tests.check(t.mem(0x0301) == 0x04, "SHX stores X AND the high byte plus one"); }
		{ cpu_test t; t.m._state->cpu.y = 1; tests.check(t.instr_cycles({0xBF, 0xFF, 0x02}) == 5, "cycles: LAX abs,Y page cross = 5"); }
		{ cpu_test t; tests.check(t.instr_cycles({0xDB, 0x00, 0x03}) == 7, "cycles: DCP abs,Y = 7"); }
		{ cpu_test t; t.m._state->cpu.x = 1; tests.check(t.instr_cycles({0x1C, 0xFF, 0x02}) == 5, "cycles: NOP abs,X page cross = 5"); }
		// clang-format on
	}

	// Validate the built-in 6502 assembler (src/assembler.cpp) by checking exact
	// encodings, addressing-mode selection, data directives, error reporting, and
	// by round-tripping an assembled program through the CPU.
	void run_assembler_tests(test_runner& tests)
	{
		{
			const auto r = assemble("LDA #$01\nSTA $0400");
			const std::vector<uint8_t> expect{0xA9, 0x01, 0x8D, 0x00, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: immediate + absolute encode");
		}
		{
			const auto r = assemble(".org $0200\nLDA $10");
			const std::vector<uint8_t> expect{0xA5, 0x10};
			tests.check(r.ok && r.origin == 0x0200 && r.bytes == expect, "ASM: zero-page selected by value");
		}
		{
			const auto r = assemble("*=$0200\nloop: INX\n CPX #$05\n BNE loop\n STX $0400");
			const std::vector<uint8_t> expect{0xE8, 0xE0, 0x05, 0xD0, 0xFB, 0x8E, 0x00, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: labels and relative branch");
		}
		{
			const auto r = assemble(".byte $01,$02\n.word $0403");
			const std::vector<uint8_t> expect{0x01, 0x02, 0x03, 0x04};
			tests.check(r.ok && r.bytes == expect, "ASM: .byte and .word emit little-endian data");
		}
		{
			const auto r = assemble("LDA #$01\nFOO $10");
			tests.check(!r.ok && r.error_line == 2, "ASM: unknown mnemonic reports the error line");
		}
		{
			// Assemble a small counting loop and run it on the CPU (relative branches
			// are position-independent, so loading at $0200 matches the *=$0200 origin).
			const auto r = assemble("*=$0200\n LDX #$00\nloop: INX\n CPX #$05\n BNE loop\n STX $0300");
			cpu_test t;
			t.run_prog(r.bytes);
			tests.check(r.ok && t.x() == 0x05 && t.mem(0x0300) == 0x05, "ASM: assembled loop runs on the CPU");
		}
	}

	// Direct VIC-II graphics validation: set up registers + video memory, render
	// a single raster line with the real renderer, and assert exact framebuffer
	// pixels / collision / raster behaviour.
	void run_vic_tests(test_runner& tests)
	{
		constexpr int W = VIC_FB_WIDTH;
		constexpr int BX = 32; // border width (left)  = (384-320)/2
		constexpr int BY = 36; // border height (top)  = (272-200)/2

		auto setup_text = [](machine& m)
		{
			io(m, 0xD011) = 0x1B; // DEN + RSEL, YSCROLL 3 (the neutral / KERNAL default)
			io(m, 0xD016) = 0x08; // CSEL, no MCM
			io(m, 0xD018) = 0x18; // screen $0400, char/bitmap base $2000 (in RAM, so we control glyphs)
			io(m, 0xD020) = 14;   // border light blue
			io(m, 0xD021) = 6;    // background blue
			io(m, 0xDD00) = 0x03; // VIC bank 0
		};

		// --- Raster counter + raster interrupt (driven directly, no CPU) ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->vic.raster_compare = 100;
			m._state->vic.irq_enable = 0x01;
			m._state->irq_pending = false;
			for (int i = 0; i < 400; ++i) m._state->tick_vic(63); // one raster line per call
			tests.check((m._state->vic.irq_status & 0x01) != 0, "VIC: raster compare sets the IRQ flag");
			tests.check((m._state->vic.irq_status & 0x80) != 0, "VIC: enabled raster IRQ asserts (bit 7)");
			tests.check(m._state->irq_pending, "VIC: raster IRQ raises the CPU IRQ line");
			m._state->write_vic(0xD019, 0x01);
			tests.check((m._state->vic.irq_status & 0x81) == 0, "VIC: writing $D019 acknowledges the raster IRQ");
		}

		// --- Standard character mode ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0x0400] = 1;                // char code 1 at screen (0,0)
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x80; // glyph row 0: leftmost pixel set
			io(m, 0xD800) = 1;                        // colour RAM = white
			vic_render_scanline(m._state, 51);        // raster 51 -> top pixel row of char row 0
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[1], "VIC: character foreground pixel");
			tests.check(fb[BY * W + BX + 1] == vic_palette[6], "VIC: character background pixel");
			tests.check(fb[BY * W + 0] == vic_palette[14], "VIC: border pixel outside the screen");
		}

		// --- Multicolor character mode ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD016) |= 0x10; // MCM
			io(m, 0xD022) = 8;     // background 1 orange
			io(m, 0xD023) = 5;     // background 2 green
			m._state->RAM[0x0400] = 1;
			io(m, 0xD800) = 0x08 | 3;                 // multicolor flag + foreground colour 3 (cyan)
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x1B; // bit-pairs 00 01 10 11
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[6], "VIC: multicolor pair 00 = background 0");
			tests.check(fb[BY * W + BX + 2] == vic_palette[8], "VIC: multicolor pair 01 = background 1");
			tests.check(fb[BY * W + BX + 4] == vic_palette[5], "VIC: multicolor pair 10 = background 2");
			tests.check(fb[BY * W + BX + 6] == vic_palette[3], "VIC: multicolor pair 11 = foreground");
		}

		// --- Standard bitmap mode ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD011) |= 0x20;            // BMM
			m._state->RAM[0x0400] = 0x1C;     // cell colours: fg=1 white, bg=12 grey
			m._state->RAM[0x2000 + 0] = 0x80; // bitmap row 0: leftmost bit set
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[1], "VIC: bitmap foreground pixel");
			tests.check(fb[BY * W + BX + 1] == vic_palette[12], "VIC: bitmap background pixel");
		}

		// --- Sprite rendering ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD015) = 0x01;              // sprite 0 enabled
			io(m, 0xD000) = 100;               // X
			io(m, 0xD001) = 60;                // Y
			m._state->RAM[0x07F8] = 0x20;      // sprite 0 pointer -> data at $0800
			m._state->RAM[0x0800] = 0x80;      // top-left pixel of the sprite
			io(m, 0xD027) = 2;                 // sprite 0 colour red
			vic_render_scanline(m._state, 61); // sprite's top row
			const auto* fb = m.framebuffer();
			const int y = 60 - 50 + BY;   // sprite_y
			const int sx = 100 - 24 + BX; // sprite_x
			tests.check(fb[y * W + sx] == vic_palette[2], "VIC: sprite pixel rendered at expected position");
		}

		// --- Sprite-to-sprite collision ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD015) = 0x03; // sprites 0 and 1
			io(m, 0xD000) = 100;
			io(m, 0xD001) = 60;
			io(m, 0xD002) = 100;
			io(m, 0xD003) = 60; // same position
			m._state->RAM[0x07F8] = 0x20;
			m._state->RAM[0x07F9] = 0x20;
			m._state->RAM[0x0800] = 0x80;
			io(m, 0xD01E) = 0;
			m._state->vic.irq_enable = 0x02; // enable the sprite-sprite collision IRQ
			vic_render_scanline(m._state, 61);
			tests.check((io(m, 0xD01E) & 0x03) == 0x03, "VIC: sprite-sprite collision flags both sprites");
			tests.check((m._state->vic.irq_status & 0x02) != 0, "VIC: sprite-sprite collision latches $D019 bit 1");
			tests.check((m._state->vic.irq_status & 0x80) != 0 && m._state->irq_pending,
			            "VIC: an enabled collision interrupt reaches the CPU");
		}

		// --- DEN ($D011 bit 4): clearing it blanks the display window ---
		{
			machine m;
			setup_text(m);
			m._state->RAM[0x0400] = 1;
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x80;
			io(m, 0xD800) = 1;
			io(m, 0xD011) &= ~0x10; // DEN off
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[14] && fb[BY * W + BX + 1] == vic_palette[14],
			            "VIC: DEN off blanks the display window to border colour");
		}

		// --- VIC bank comes from CIA2 port A through its data direction register ---
		{
			machine m;
			setup_text(m);
			io(m, 0xDD00) = 0x00; // latch says bank 3, but every line is an input
			io(m, 0xDD02) = 0x00;
			m._state->RAM[0x0400] = 1;
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0x80;
			io(m, 0xD800) = 1;
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 0] == vic_palette[1],
			            "VIC: undriven CIA2 port A lines float high, selecting bank 0");
		}

		// --- Sprite-to-background collision (foreground vs blank) ---
		{
			machine m;
			setup_text(m);
			for (int i = 0; i < 8; ++i) m._state->RAM[0x2000 + 1 * 8 + i] = 0xFF; // solid glyph
			for (int c = 0; c < 40; ++c) m._state->RAM[0x0400 + 40 + c] = 1;      // fill char row 1
			io(m, 0xD015) = 0x01;
			io(m, 0xD000) = 100;
			io(m, 0xD001) = 60;
			m._state->RAM[0x07F8] = 0x20;
			m._state->RAM[0x0800] = 0x80;
			io(m, 0xD01F) = 0;
			vic_render_scanline(m._state, 61);
			tests.check((io(m, 0xD01F) & 0x01) != 0, "VIC: sprite collides with foreground graphics");
		}
		{
			machine m;
			setup_text(m); // blank screen (char 0, empty glyph) = background only
			io(m, 0xD015) = 0x01;
			io(m, 0xD000) = 100;
			io(m, 0xD001) = 60;
			m._state->RAM[0x07F8] = 0x20;
			m._state->RAM[0x0800] = 0x80;
			io(m, 0xD01F) = 0;
			vic_render_scanline(m._state, 61);
			tests.check((io(m, 0xD01F) & 0x01) == 0, "VIC: no sprite-background collision over blank screen");
		}

		// --- Register read-back semantics (inspired by VICE's VIC-II register
		//     tests): unused bits read as 1, mirroring, raster/collision reads ---
		{
			machine m;
			m._state->vic = vic_chip{};
			// Unused bits are forced to 1 on read regardless of what was written.
			m._state->write_vic(0xD016, 0x00);
			tests.check((m._state->read_vic(0xD016) & 0xC0) == 0xC0, "VIC: $D016 unused bits 6-7 read as 1");
			m._state->write_vic(0xD018, 0x00);
			tests.check((m._state->read_vic(0xD018) & 0x01) == 0x01, "VIC: $D018 unused bit 0 reads as 1");
			m._state->write_vic(0xD01A, 0x00);
			tests.check((m._state->read_vic(0xD01A) & 0xF0) == 0xF0, "VIC: $D01A unused bits 4-7 read as 1");
			m._state->write_vic(0xD020, 0x00);
			tests.check((m._state->read_vic(0xD020) & 0xF0) == 0xF0, "VIC: $D020 colour high nibble reads as 1");
			tests.check(m._state->read_vic(0xD02F) == 0xFF, "VIC: non-existent $D02F reads as $FF");

			// The chip mirrors its 64-byte register block through $D3FF.
			m._state->write_vic(0xD021, 0x0A);
			tests.check((m._state->read_vic(0xD061) & 0x0F) == 0x0A,
			            "VIC: register block mirrors at $D040+ ($D061 == $D021)");
			m._state->write_vic(0xD861, 0x0C); // mirror write lands in $D021
			tests.check((m._state->read_vic(0xD021) & 0x0F) == 0x0C, "VIC: mirror writes reach the real register");

			// Collision registers are read-only and clear themselves on read.
			io(m, 0xD01E) = 0x05;
			tests.check(m._state->read_vic(0xD01E) == 0x05, "VIC: $D01E returns the collision mask");
			tests.check(m._state->read_vic(0xD01E) == 0x00, "VIC: $D01E clears itself after a read");
			io(m, 0xD01F) = 0x03;
			m._state->write_vic(0xD01F, 0xFF); // write must be ignored
			tests.check(m._state->read_vic(0xD01F) == 0x03, "VIC: $D01F is read-only");
		}

		// --- Raster counter read-back ($D011 bit 7 = MSB, $D012 = low byte) ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->write_vic(0xD011, 0x00);
			for (int i = 0; i < 260; ++i) m._state->tick_vic(63); // advance to raster line 260
			tests.check(m._state->read_vic(0xD012) == (260 & 0xFF), "VIC: $D012 returns the raster line low byte");
			tests.check((m._state->read_vic(0xD011) & 0x80) == 0x80, "VIC: $D011 bit 7 returns the raster line MSB");
		}

		// --- $D019 IRQ latch: bit 7 summarises enabled+pending, write acks ---
		{
			machine m;
			m._state->vic = vic_chip{};
			m._state->vic.raster_compare = 50;
			m._state->write_vic(0xD01A, 0x01); // enable raster IRQ
			for (int i = 0; i < 60; ++i) m._state->tick_vic(63);
			const uint8_t status = m._state->read_vic(0xD019);
			tests.check((status & 0x01) == 0x01, "VIC: $D019 bit 0 latches the raster IRQ");
			tests.check((status & 0x80) == 0x80, "VIC: $D019 bit 7 summarises an enabled IRQ");
			tests.check((status & 0x70) == 0x70, "VIC: $D019 unused bits 4-6 read as 1");
			m._state->write_vic(0xD019, 0x01); // acknowledge
			tests.check((m._state->read_vic(0xD019) & 0x81) == 0x00,
			            "VIC: writing $D019 acknowledges the latch and clears bit 7");
		}

		// --- CSEL clear: the 38-column border covers seven pixels on the left ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD016) = 0x00; // 38 columns
			m._state->RAM[0x0400] = 1;
			m._state->RAM[0x2000 + 1 * 8 + 0] = 0xFF;
			io(m, 0xD800) = 1;
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 6] == vic_palette[14] && fb[BY * W + BX + 7] == vic_palette[1],
			            "VIC: 38 columns draws the border over the first seven pixels");
			tests.check(fb[BY * W + BX + 310] == vic_palette[6] && fb[BY * W + BX + 311] == vic_palette[14],
			            "VIC: 38 columns draws the border over the last nine pixels");
		}

		// --- RSEL clear: the 24-row border is border, not background ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD011) = 0x13; // 24 rows
			vic_render_scanline(m._state, 51);
			vic_render_scanline(m._state, 55);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 10] == vic_palette[14] && fb[(BY + 4) * W + BX + 10] == vic_palette[6],
			            "VIC: 24 rows draws the top four lines of the window as border");
		}

		// --- The side border covers a sprite ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD015) = 0x01;
			io(m, 0xD000) = 10;
			io(m, 0xD001) = 60; // X 10: under the left border
			m._state->RAM[0x07F8] = 0x20;
			m._state->RAM[0x0800] = 0xFF;
			m._state->RAM[0x0801] = 0xFF;
			io(m, 0xD027) = 2;
			vic_render_scanline(m._state, 61);
			const auto* fb = m.framebuffer();
			const int y = 60 - 50 + BY;
			tests.check(fb[y * W + 10 - 24 + BX + 2] == vic_palette[14] && fb[y * W + BX + 1] == vic_palette[2],
			            "VIC: the side border hides the part of a sprite under it");
		}

		// --- A sprite past the right edge wraps round to the left ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD015) = 0x01;
			io(m, 0xD000) = 0xF0;
			io(m, 0xD010) = 0x01;
			io(m, 0xD001) = 60; // X $1F0
			m._state->RAM[0x07F8] = 0x20;
			m._state->RAM[0x0800] = 0x80;
			io(m, 0xD027) = 2;
			io(m, 0xD016) = 0x08;
			// Open the left border's colour to tell the sprite from it: it is drawn
			// over by the border, so look at the collision buffer instead.
			vic_render_scanline(m._state, 61);
			tests.check(m._state->sprite_collision[0] == 0, "VIC: a sprite at X $1F0 is drawn at the left edge");
		}

		// --- Invalid modes draw black graphics ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD011) = 0x5B; // ECM on
			io(m, 0xD016) = 0x18; // and MCM: invalid
			vic_render_scanline(m._state, 51);
			const auto* fb = m.framebuffer();
			tests.check(fb[BY * W + BX + 100] == vic_palette[0] && fb[BY * W + 4] == vic_palette[14],
			            "VIC: an invalid mode draws the window black");
		}

		// --- Sprite data at $1000 of bank 0 comes from the character ROM ---
		{
			machine m;
			setup_text(m);
			io(m, 0xD015) = 0x01;
			io(m, 0xD000) = 100;
			io(m, 0xD001) = 60;
			m._state->RAM[0x07F8] = 0x40; // $1000
			io(m, 0xD027) = 2;
			vic_render_scanline(m._state, 61);
			const auto* fb = m.framebuffer();
			const int y = 60 - 50 + BY, sx = 100 - 24 + BX;
			// The first byte of the ROM is the top of '@': %00111100.
			tests.check(fb[y * W + sx + 1] != vic_palette[2] && fb[y * W + sx + 2] == vic_palette[2],
			            "VIC: a sprite fetched from $1000 in bank 0 reads the character ROM");
		}

		// --- Bad lines and sprite fetches take cycles from the CPU ---
		{
			const auto cpu_cycles_in_frame = [](const uint8_t d011, const uint8_t sprites)
			{
				machine m;
				m._state->raw_ram = true;
				m._state->RAM[0x0200] = 0xE8; // INX
				m._state->RAM[0x0201] = 0x4C; // JMP $0200
				m._state->RAM[0x0202] = 0x00;
				m._state->RAM[0x0203] = 0x02;
				m._state->cpu.pc = 0x0200;
				io(m, 0xD011) = d011;
				io(m, 0xD015) = sprites;
				for (int s = 0; s < 8; ++s) io(m, static_cast<uint16_t>(0xD001 + s * 2)) = 100;
				m.exec(pal_cycles_per_frame * 2); // a frame to latch DEN on raster $30
				const int64_t before = m._state->instruction_count;
				m.exec(pal_cycles_per_frame);
				return (m._state->instruction_count - before) * 5 / 2; // INX + JMP = 5 cycles per pair
			};
			const auto blank = cpu_cycles_in_frame(0x0B, 0);
			const auto text = cpu_cycles_in_frame(0x1B, 0);
			const auto sprites = cpu_cycles_in_frame(0x0B, 0xFF);
			std::cout << "  CPU cycles a frame: blank " << blank << ", text " << text << ", 8 sprites " << sprites
					  << '\n';
			tests.check(blank - text > 950 && blank - text < 1050, "VIC: 25 bad lines take about 1000 cycles a frame");
			tests.check(blank - sprites > 21 * 20 && blank - sprites < 21 * 24,
			            "VIC: eight sprites take 22 cycles a line");
		}
	}

	// CIA timer validation (inspired by VICE's CIA timer test programs): counting,
	// latch reload on underflow, ICR flags, one-shot vs continuous, force-load,
	// interrupt masking and the Timer-B-counts-Timer-A cascade. Driven directly
	// through tick_cia so the results are deterministic and CPU-independent.
	void run_cia_tests(test_runner& tests)
	{
		// Start Timer A from a known latch in continuous mode.
		auto arm_timer_a = [](machine& m, uint16_t latch, uint8_t cra)
		{
			m._state->cia1 = cia_chip{};
			m._state->irq_pending = false;
			m._state->write_cia1(0xDC04, static_cast<uint8_t>(latch & 0xFF));
			m._state->write_cia1(0xDC05, static_cast<uint8_t>(latch >> 8)); // loads counter (timer stopped)
			m._state->write_cia1(0xDC0E, cra);
		};

		// --- Counting down ---
		{
			machine m;
			arm_timer_a(m, 100, 0x01);
			m._state->tick_cia(m._state->cia1, 40, false);
			tests.check(m._state->read_cia1(0xDC04) == 60, "CIA: Timer A counts down with the system clock");
		}

		// --- Underflow reloads from the latch and sets the ICR flag ---
		{
			machine m;
			arm_timer_a(m, 10, 0x01);
			m._state->tick_cia(m._state->cia1, 11, false); // 10 - 11 underflows, +latch = 9
			tests.check(m._state->read_cia1(0xDC04) == 9, "CIA: Timer A reloads from the latch after underflow");
			tests.check((m._state->cia1.icr_data & 0x01) != 0, "CIA: Timer A underflow sets ICR bit 0");
		}

		// --- Reading the ICR returns the flags and clears them ---
		{
			machine m;
			arm_timer_a(m, 10, 0x01);
			m._state->tick_cia(m._state->cia1, 11, false);
			const uint8_t first = m._state->read_cia1(0xDC0D);
			const uint8_t second = m._state->read_cia1(0xDC0D);
			tests.check((first & 0x01) != 0, "CIA: reading $DC0D reports the Timer A flag");
			tests.check(second == 0x00, "CIA: reading $DC0D clears the pending flags");
		}

		// --- One-shot mode stops after a single underflow ---
		{
			machine m;
			arm_timer_a(m, 5, 0x09); // bit3 = one-shot, bit0 = start
			m._state->tick_cia(m._state->cia1, 5, false);
			tests.check(!m._state->cia1.timer_a_running, "CIA: one-shot Timer A stops after underflow");
			tests.check((m._state->cia1.cra & 0x01) == 0, "CIA: one-shot clears the start bit in $DC0E");
		}

		// --- Continuous mode keeps running across an underflow ---
		{
			machine m;
			arm_timer_a(m, 5, 0x01);
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check(m._state->cia1.timer_a_running, "CIA: continuous Timer A keeps running after underflow");
		}

		// --- Force-load (bit 4) reloads the counter from the latch immediately ---
		{
			machine m;
			arm_timer_a(m, 100, 0x01);
			m._state->tick_cia(m._state->cia1, 40, false); // counter now 60
			m._state->write_cia1(0xDC0E, 0x11);            // start + force-load
			tests.check(m._state->read_cia1(0xDC04) == 100, "CIA: writing $DC0E bit 4 force-loads the latch");
		}

		// --- Interrupt masking: enabled underflow raises the IRQ line ---
		{
			machine m;
			arm_timer_a(m, 5, 0x01);
			m._state->write_cia1(0xDC0D, 0x81); // set-mask + Timer A enable
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check((m._state->cia1.icr_data & 0x80) != 0, "CIA: enabled Timer A underflow sets ICR bit 7");
			tests.check(m._state->irq_pending, "CIA: enabled Timer A underflow raises the CPU IRQ line");
		}

		// --- Interrupt masking: a disabled timer flags but does not interrupt ---
		{
			machine m;
			arm_timer_a(m, 5, 0x01); // interrupts left disabled
			m._state->tick_cia(m._state->cia1, 6, false);
			tests.check((m._state->cia1.icr_data & 0x01) != 0, "CIA: disabled Timer A still latches its flag");
			tests.check((m._state->cia1.icr_data & 0x80) == 0 && !m._state->irq_pending,
			            "CIA: disabled Timer A does not interrupt");
		}

		// --- Timer B cascade: CRB bits 5-6 = 01 counts Timer A underflows ---
		{
			machine m;
			m._state->cia1 = cia_chip{};
			m._state->irq_pending = false;
			m._state->write_cia1(0xDC06, 2);
			m._state->write_cia1(0xDC07, 0); // Timer B latch = 2
			m._state->write_cia1(0xDC04, 2);
			m._state->write_cia1(0xDC05, 0);    // Timer A latch = 2
			m._state->write_cia1(0xDC0F, 0x41); // Timer B: count Timer A underflows + start
			m._state->write_cia1(0xDC0E, 0x01); // Timer A: start
			for (int i = 0; i < 3; ++i) m._state->tick_cia(m._state->cia1, 2, false); // 3 Timer A underflows
			tests.check((m._state->cia1.icr_data & 0x02) != 0,
			            "CIA: Timer B underflows after counting Timer A underflows");
		}

		// --- Timer A counts the CNT pin, which nothing drives, when CRA bit 5 is set ---
		{
			machine m;
			arm_timer_a(m, 100, 0x21);
			m._state->tick_cia(m._state->cia1, 40, false);
			tests.check(m._state->read_cia1(0xDC04) == 100, "CIA: Timer A counting CNT stands still");
		}

		// --- Time of day: stopped from reset, started by writing the tenths ---
		{
			machine m;
			auto& s = *m._state;
			s.write_cia1(0xDC0E, 0x80); // 50 Hz mains
			s.write_cia1(0xDC0B, 0x11); // 11 o'clock: stops the clock
			s.write_cia1(0xDC0A, 0x59);
			s.write_cia1(0xDC09, 0x59);
			s.tick_cia(s.cia1, 985248, false);
			tests.check(s.read_cia1(0xDC09) == 0x59, "CIA: writing the TOD hours stops the clock");
			s.write_cia1(0xDC08, 0x08);            // .8 seconds: starts it
			s.tick_cia(s.cia1, 985248 / 2, false); // half a second
			const uint8_t hours = s.read_cia1(0xDC0B);
			s.tick_cia(s.cia1, 985248, false);
			const uint8_t minutes = s.read_cia1(0xDC0A);
			const uint8_t seconds = s.read_cia1(0xDC09);
			const uint8_t tenths = s.read_cia1(0xDC08);
			tests.check(hours == 0x92 && minutes == 0x00 && seconds == 0x00 && tenths == 0x03,
			            "CIA: reading the TOD hours latches the time until the tenths are read");
			tests.check(s.read_cia1(0xDC09) == 0x01, "CIA: the TOD clock runs on once the tenths are read");
		}

		// --- Time of day: the alarm interrupts ---
		{
			machine m;
			auto& s = *m._state;
			s.write_cia1(0xDC0E, 0x80);
			s.write_cia1(0xDC0F, 0x80); // writes set the alarm
			s.write_cia1(0xDC0B, 0x01);
			s.write_cia1(0xDC0A, 0x00);
			s.write_cia1(0xDC09, 0x00);
			s.write_cia1(0xDC08, 0x05);
			s.write_cia1(0xDC0F, 0x00); // and now the clock
			s.write_cia1(0xDC0B, 0x01);
			s.write_cia1(0xDC0A, 0x00);
			s.write_cia1(0xDC09, 0x00);
			s.write_cia1(0xDC08, 0x00);
			s.write_cia1(0xDC0D, 0x84); // enable the alarm interrupt
			s.irq_pending = false;
			s.tick_cia(s.cia1, 985248 / 10 * 4, false);
			const bool early = s.irq_pending;
			s.tick_cia(s.cia1, 985248 / 10 * 2, false);
			tests.check(!early && s.irq_pending && (s.cia1.icr_data & 0x84) == 0x84,
			            "CIA: the TOD alarm raises an interrupt when the time reaches it");
		}
	}

	// ======================================================================
	// SID audio quality: the chip's output measured the way a listener would
	// notice it going wrong.
	// ======================================================================

	constexpr double test_pi = 3.14159265358979323846;

	// Power at each bin of a Blackman-Harris windowed signal, by an in-place
	// radix-2 FFT. The window's side lobes are 92 dB down, below anything the
	// checks measure. samples.size() must be a power of two.
	std::vector<double> power_spectrum(const std::vector<double>& samples)
	{
		const size_t n = samples.size();
		std::vector<double> re(n), im(n, 0.0);
		for (size_t i = 0; i < n; ++i)
		{
			const double t = 2.0 * test_pi * static_cast<double>(i) / static_cast<double>(n - 1);
			const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
			re[i] = samples[i] * w;
		}
		for (size_t i = 1, j = 0; i < n; ++i)
		{
			size_t bit = n >> 1;
			for (; j & bit; bit >>= 1) j ^= bit;
			j ^= bit;
			if (i < j) std::swap(re[i], re[j]);
		}
		for (size_t len = 2; len <= n; len <<= 1)
		{
			for (size_t i = 0; i < n; i += len)
			{
				for (size_t k = 0; k < len / 2; ++k)
				{
					const double angle = -2.0 * test_pi * static_cast<double>(k) / static_cast<double>(len);
					const double c = std::cos(angle), s = std::sin(angle);
					const size_t a = i + k, b = a + len / 2;
					const double vr = re[b] * c - im[b] * s;
					const double vi = re[b] * s + im[b] * c;
					re[b] = re[a] - vr;
					im[b] = im[a] - vi;
					re[a] += vr;
					im[a] += vi;
				}
			}
		}
		std::vector<double> power(n / 2);
		for (size_t i = 0; i < n / 2; ++i) power[i] = re[i] * re[i] + im[i] * im[i];
		return power;
	}

	// Voice 1 programmed straight into the chip while the CPU idles, so nothing
	// but the chip shapes what is heard. Settles for a quarter of a second, then
	// returns count samples at 44.1 kHz.
	std::vector<double> render_voice(const sid_model model, const uint16_t frequency, const uint8_t control,
	                                 const uint8_t res_filt, const uint8_t mode_vol, const uint16_t cutoff,
	                                 const size_t count)
	{
		machine m;
		m._state->raw_ram = true;
		m._state->RAM[0x0200] = 0x4C; // JMP $0200
		m._state->RAM[0x0201] = 0x00;
		m._state->RAM[0x0202] = 0x02;
		m._state->cpu.pc = 0x0200;
		m.set_sid_model(model);
		m.set_audio_enabled(true);

		auto& sid = m._state->sid;
		sid.write_now(0x15, static_cast<uint8_t>(cutoff & 0x07));
		sid.write_now(0x16, static_cast<uint8_t>(cutoff >> 3));
		sid.write_now(0x17, res_filt);
		sid.write_now(0x18, mode_vol);
		sid.write_now(0x00, static_cast<uint8_t>(frequency & 0xFF));
		sid.write_now(0x01, static_cast<uint8_t>(frequency >> 8));
		sid.write_now(0x02, 0x00);
		sid.write_now(0x03, 0x08); // 50% pulse width
		sid.write_now(0x05, 0x00);
		sid.write_now(0x06, 0xF0);
		sid.write_now(0x04, control);

		std::vector<double> out;
		const size_t settle = 11025;
		int16_t buffer[4096];
		while (out.size() < settle + count)
		{
			m.exec(pal_cycles_per_frame);
			const int n = m.generate_audio(buffer, 4096);
			out.insert(out.end(), buffer, buffer + n);
		}
		out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(settle));
		out.resize(count);
		return out;
	}

	double rms(const std::vector<double>& samples)
	{
		double sum = 0;
		for (const double s : samples) sum += s * s;
		return std::sqrt(sum / static_cast<double>(samples.size()));
	}

	// A PSID image with one tune: init plays a 1 kHz (at the tune's clock)
	// triangle on voice 1, and play counts its calls at $C000.
	std::vector<uint8_t> make_test_psid(const bool ntsc, const bool cia_timed)
	{
		const double clock = ntsc ? sid_clock_ntsc : sid_clock_pal;
		const auto freq = static_cast<uint16_t>(std::lround(1000.0 * 16777216.0 / clock));

		std::vector<uint8_t> file(0x7C, 0);
		const auto be16 = [&](const size_t at, const uint16_t v)
		{
			file[at] = static_cast<uint8_t>(v >> 8);
			file[at + 1] = static_cast<uint8_t>(v & 0xFF);
		};
		std::memcpy(file.data(), "PSID", 4);
		be16(0x04, 2);      // version
		be16(0x06, 0x7C);   // data offset
		be16(0x08, 0x1000); // load
		be16(0x0A, 0x1000); // init
		be16(0x0C, 0x1020); // play
		be16(0x0E, 1);      // songs
		be16(0x10, 1);      // start song
		file[0x15] = cia_timed ? 1 : 0;
		std::memcpy(file.data() + 0x16, "CLOCK TEST", 10);
		be16(0x76, static_cast<uint16_t>((ntsc ? 0x08 : 0x04) | 0x10)); // clock, 6581

		// clang-format off
		const uint8_t code[] = {
			0xA9, 0x0F, 0x8D, 0x18, 0xD4,                                  // volume 15
			0xA9, static_cast<uint8_t>(freq & 0xFF), 0x8D, 0x00, 0xD4,     // frequency
			0xA9, static_cast<uint8_t>(freq >> 8), 0x8D, 0x01, 0xD4,
			0xA9, 0x00, 0x8D, 0x05, 0xD4,                                  // attack/decay
			0xA9, 0xF0, 0x8D, 0x06, 0xD4,                                  // sustain/release
			0xA9, 0x11, 0x8D, 0x04, 0xD4,                                  // triangle, gate
			0x60,                                                          // RTS
		};
		// clang-format on
		file.insert(file.end(), std::begin(code), std::end(code));
		file.resize(0x7C + 0x20, 0xEA);
		const uint8_t play[] = {0xEE, 0x00, 0xC0, 0x60}; // INC $C000; RTS
		file.insert(file.end(), std::begin(play), std::end(play));
		return file;
	}

	void run_sid_audio_tests(test_runner& tests)
	{
		constexpr size_t n = 16384;
		constexpr double bin_hz = 44100.0 / n;

		// A sawtooth's harmonics run far past 22 kHz, and any the resampler lets
		// through fold back between the real ones as inharmonic grit. The chip
		// computes its waveforms once a cycle, so it has aliases of its own clock;
		// a period of exactly 512 cycles puts those on the harmonics, leaving only
		// what the resampler adds between them.
		{
			constexpr uint16_t frequency = 0x8000; // 512 cycles: 1924 Hz
			const double f0 = frequency * static_cast<double>(sid_clock_pal) / 16777216.0;
			const auto power = power_spectrum(render_voice(sid_model::mos8580, frequency, 0x21, 0x00, 0x0F, 0, n));
			double harmonic = 0, other = 0;
			for (size_t bin = static_cast<size_t>(100 / bin_hz); bin < static_cast<size_t>(19000 / bin_hz); ++bin)
			{
				const double hz = static_cast<double>(bin) * bin_hz;
				const double nearest = std::round(hz / f0) * f0;
				if (nearest > 0 && std::abs(hz - nearest) <= 8 * bin_hz) harmonic += power[bin];
				else other += power[bin];
			}
			const double aliases_db = 10.0 * std::log10(other / harmonic);
			std::cout << "  sawtooth aliases: " << aliases_db << " dB\n";
			tests.check(aliases_db < -60.0, "SID: a high sawtooth's aliases stay 60 dB below its harmonics");
		}

		// The filter's resonance shapes the peak; it must not turn the pass band down.
		{
			constexpr uint16_t frequency = 3746; // 220 Hz
			const double flat = rms(render_voice(sid_model::mos8580, frequency, 0x21, 0x01, 0x1F, 2047, n));
			const double resonant = rms(render_voice(sid_model::mos8580, frequency, 0x21, 0xF1, 0x1F, 2047, n));
			const double change_db = 20.0 * std::log10(resonant / flat);
			std::cout << "  resonance 15 vs 0 in the pass band: " << change_db << " dB\n";
			tests.check(std::abs(change_db) < 1.5, "SID: full resonance leaves the filter's pass band level");
		}

		// The cutoff curves cover the ranges the chips are measured at.
		{
			bool rising = true;
			for (int fc = 1; fc < 2048; ++fc)
				rising = rising && sid_filter::cutoff_hz(true, fc) > sid_filter::cutoff_hz(true, fc - 1) &&
				         sid_filter::cutoff_hz(false, fc) > sid_filter::cutoff_hz(false, fc - 1);
			tests.check(rising, "SID: both cutoff curves rise with FC");
			tests.check(sid_filter::cutoff_hz(true, 0) > 180 && sid_filter::cutoff_hz(true, 0) < 300 &&
			                sid_filter::cutoff_hz(true, 2047) > 15000 && sid_filter::cutoff_hz(true, 1024) > 4000 &&
			                sid_filter::cutoff_hz(true, 1024) < 8000,
			            "SID: the 6581 filter never closes below 200 Hz and opens past 15 kHz");
			tests.check(sid_filter::cutoff_hz(false, 0) < 50 && sid_filter::cutoff_hz(false, 2047) > 12000 &&
			                sid_filter::cutoff_hz(false, 2047) < 13000,
			            "SID: the 8580 filter runs from near 0 Hz to 12.5 kHz");
		}

		// A full-scale voice sits well inside 16 bits, leaving room for three.
		{
			const auto saw = render_voice(sid_model::mos8580, 3746, 0x21, 0x00, 0x0F, 0, n);
			double peak = 0;
			for (const double s : saw) peak = std::max(peak, std::abs(s));
			std::cout << "  full-scale sawtooth peak: " << peak << '\n';
			tests.check(peak > 5000 && peak < 9000, "SID: a full-scale voice peaks near a quarter of full scale");
		}

		// An NTSC tune runs at the NTSC clock (its pitch) and frame rate (its tempo).
		for (const bool ntsc : {false, true})
		{
			const auto image = make_test_psid(ntsc, false);
			machine m;
			const bool loaded = m.load_sid(image.data(), image.size());
			const double clock = ntsc ? sid_clock_ntsc : sid_clock_pal;
			const uint8_t calls_before = m._state->RAM[0xC000];
			std::vector<double> pcm;
			int16_t buffer[4096];
			int64_t cycles = 0;
			while (cycles < static_cast<int64_t>(clock))
			{
				m.exec(pal_cycles_per_frame);
				cycles += pal_cycles_per_frame;
				const int got = m.generate_audio(buffer, 4096);
				pcm.insert(pcm.end(), buffer, buffer + got);
			}
			const int calls = static_cast<uint8_t>(m._state->RAM[0xC000] - calls_before);

			// Zero crossings over the second half: twice the tone's frequency.
			int crossings = 0;
			for (size_t i = pcm.size() / 2 + 1; i < pcm.size(); ++i)
				if ((pcm[i - 1] < 0) != (pcm[i] < 0)) ++crossings;
			const double hz = crossings / 2.0 / (static_cast<double>(pcm.size() / 2) / 44100.0);
			std::cout << "  " << (ntsc ? "NTSC" : "PAL") << " tune: " << calls << " play calls, " << hz << " Hz\n";

			const int rate = ntsc ? 60 : 50;
			tests.check(loaded && calls >= rate - 1 && calls <= rate + 1,
			            ntsc ? "SID: an NTSC tune's play routine runs 60 times a second"
			                 : "SID: a PAL tune's play routine runs 50 times a second");
			tests.check(hz > 990 && hz < 1010,
			            ntsc ? "SID: an NTSC tune sounds at its own pitch" : "SID: a PAL tune sounds at its own pitch");
		}
	}

	// Render a tune to a 16-bit mono WAV for listening or analysis:
	// --render-sid <file.sid> <out.wav> [seconds] [song]
	int render_sid(const std::filesystem::path& path, const std::filesystem::path& out, const double seconds,
	               const int song)
	{
		const auto bytes = read_file(path);
		machine m;
		if (!m.load_sid(bytes.data(), bytes.size(), song))
		{
			std::cerr << "Not a SID tune: " << path.string() << '\n';
			return 2;
		}

		const auto rate = m.audio_sample_rate();
		const auto wanted = static_cast<size_t>(seconds * rate);
		std::vector<int16_t> pcm;
		int16_t buffer[4096];
		while (pcm.size() < wanted)
		{
			m.exec(pal_cycles_per_frame);
			const int got = m.generate_audio(buffer, 4096);
			pcm.insert(pcm.end(), buffer, buffer + got);
		}
		pcm.resize(wanted);

		std::ofstream file(out, std::ios::binary);
		const auto u32 = [&](const uint32_t v)
		{
			file.write(reinterpret_cast<const char*>(&v), 4);
		};
		const auto u16 = [&](const uint16_t v)
		{
			file.write(reinterpret_cast<const char*>(&v), 2);
		};
		const auto data_bytes = static_cast<uint32_t>(pcm.size() * 2);
		file.write("RIFF", 4);
		u32(36 + data_bytes);
		file.write("WAVEfmt ", 8);
		u32(16);
		u16(1);
		u16(1);
		u32(rate);
		u32(rate * 2);
		u16(2);
		u16(16);
		file.write("data", 4);
		u32(data_bytes);
		file.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
		return file ? 0 : 1;
	}

	// Screen code -> ASCII for dumping the text screen of a .prg test.
	char screen_char(const uint8_t code)
	{
		const uint8_t c = code & 0x7F;
		if (c >= 0x01 && c <= 0x1A) return static_cast<char>('A' + c - 1);
		if (c == 0x00) return '@';
		if (c >= 0x30 && c <= 0x3F) return static_cast<char>(c); // digits + punctuation
		if (c == 0x20) return ' ';
		return (c >= 0x20 && c <= 0x5F) ? static_cast<char>(c) : '.';
	}

	void print_screen(const machine& c64)
	{
		const auto* screen = c64._state->RAM + text_video_mem_offset;
		std::cout << "+" << std::string(40, '-') << "+\n";
		for (int row = 0; row < 25; ++row)
		{
			std::cout << '|';
			for (int col = 0; col < 40; ++col) std::cout << screen_char(screen[row * 40 + col]);
			std::cout << "|\n";
		}
		std::cout << "+" << std::string(40, '-') << "+\n";
	}

	// Mount a disk image, boot, and LOAD a file from it through the kernal the way
	// a user (or a game's loader) would, then print the resulting screen.
	int run_disk(const std::filesystem::path& path, const std::string& name)
	{
		const auto bytes = read_file(path);
		machine m;
		if (!m.insert_disk(bytes.data(), bytes.size()))
		{
			std::cerr << "Not a recognised disk image: " << path.string() << '\n';
			return 2;
		}

		if (!run_until_screen_contains(m, "READY.", boot_cycle_limit))
		{
			std::cerr << "Machine did not boot.\n";
			return 1;
		}

		type_line(m, "LOAD\"" + name + "\",8,1");
		for (int i = 0; i < 40; ++i) m.exec(execution_step);
		type_line(m, "RUN");
		for (int i = 0; i < 400; ++i) m.exec(50'000);

		print_screen(m);
		return 0;
	}

	// Run a raw binary as a bare 6502 program (e.g. Klaus Dormann's 6502
	// functional test). Detects the success/failure infinite-loop trap.
	int run_bin(const std::filesystem::path& path, const uint16_t load, const uint16_t start, const bool has_expected,
	            const uint16_t expected)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			std::cerr << "Unable to read binary: " << path.string() << '\n';
			return 2;
		}

		machine m;
		m._state->raw_ram = true;
		std::fill(std::begin(m._state->RAM), std::end(m._state->RAM), static_cast<uint8_t>(0));
		const size_t room = machine_state::RAM_SIZE - load;
		const size_t count = std::min(bytes.size(), room);
		std::copy_n(bytes.begin(), count, m._state->RAM + load);

		m._state->cpu.pc = start;
		m._state->cpu.sp = 0xFD;
		m._state->cpu.a = m._state->cpu.x = m._state->cpu.y = 0;
		m._state->cpu.status = FLAG_CONSTANT | FLAG_INTERRUPT;

		constexpr long long instruction_cap = 200'000'000;
		for (long long i = 0; i < instruction_cap; ++i)
		{
			const uint16_t before = m._state->cpu.pc;
			m.exec(1);
			if (m._state->cpu.pc == before)
			{
				std::cout << "Trapped at $" << std::hex << before << std::dec << " after "
						  << m._state->instruction_count << " instructions.\n";
				if (has_expected)
				{
					const bool ok = before == expected;
					std::cout << (ok ? "PASS" : "FAIL") << " (expected $" << std::hex << expected << std::dec << ")\n";
					return ok ? 0 : 1;
				}
				return 0;
			}
		}
		std::cout << "No trap after " << instruction_cap << " instructions (PC=$" << std::hex << m._state->cpu.pc
				  << std::dec << ")\n";
		return 1;
	}

	// Load and run a .prg, then print the text screen (for eyeballing the result
	// of Lorenz / VICE single-file test programs).
	int run_prg_dump(const std::filesystem::path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.size() < 3)
		{
			std::cerr << "Unable to read PRG: " << path.string() << '\n';
			return 2;
		}

		machine m;
		for (int c = 0; c < 60 && !screen_contains(m, "READY."); ++c) m.exec(execution_step);
		m.exec(3'000'000);
		if (!m.load_prg(bytes.data(), bytes.size()))
		{
			std::cerr << "Not a valid PRG.\n";
			return 2;
		}
		for (int i = 0; i < 400; ++i) m.exec(50'000); // ~20M cycles

		print_screen(m);
		return 0;
	}

	// Start a disk, tape, cartridge or program as a user would, run it for a while
	// and save the picture as a BMP: --snap <file> <out.bmp> [seconds] [name].
	// A disk or tape starts the named file, or its first program.
	int snap(const std::filesystem::path& path, const std::filesystem::path& out, const double seconds,
	         const std::string& name)
	{
		const auto bytes = read_file(path);
		auto ext = path.extension().string();
		for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		machine m;
		m.set_render_enabled(true);
		bool started = false;
		if (ext == ".crt")
		{
			started = m.load_crt(bytes.data(), bytes.size());
		}
		else if (ext == ".t64")
		{
			std::vector<uint8_t> prg;
			started = read_tape_file(bytes.data(), bytes.size(), name.empty() ? "*" : name, prg) &&
			          m.autostart_prg(prg.data(), prg.size());
		}
		else if (disk_directory dir; read_disk_directory(bytes.data(), bytes.size(), dir))
		{
			for (const auto& entry : dir.entries)
			{
				const bool wanted = name.empty() ? entry.type == "PRG" : entry.filename.starts_with(name);
				if (!wanted) continue;
				started = m.autostart_disk(bytes.data(), bytes.size(), entry.raw_name.data(), entry.raw_name.size());
				break;
			}
		}
		else
		{
			started = m.autostart_prg(bytes.data(), bytes.size());
		}
		if (!started)
		{
			std::cerr << "Could not start " << path.string() << '\n';
			return 2;
		}

		int frames = 0;
		while (m.autostarting() && frames++ < 1000) m.exec(pal_cycles_per_frame);
		for (int frame = 0; frame < static_cast<int>(seconds * 50); ++frame) m.exec(pal_cycles_per_frame);

		constexpr int w = machine::framebuffer_width(), h = machine::framebuffer_height();
		std::ofstream file(out, std::ios::binary);
		const auto u32 = [&](const uint32_t v)
		{
			file.write(reinterpret_cast<const char*>(&v), 4);
		};
		const auto u16 = [&](const uint16_t v)
		{
			file.write(reinterpret_cast<const char*>(&v), 2);
		};
		file.write("BM", 2);
		u32(54 + w * h * 3);
		u32(0);
		u32(54);
		u32(40);
		u32(w);
		u32(h);
		u16(1);
		u16(24);
		u32(0);
		u32(w * h * 3);
		u32(2835);
		u32(2835);
		u32(0);
		u32(0);
		const auto* pixels = m.framebuffer();
		for (int y = h - 1; y >= 0; --y)
			for (int x = 0; x < w; ++x)
			{
				const uint32_t p = pixels[y * w + x];
				const char bgr[3] = {static_cast<char>(p & 0xFF), static_cast<char>((p >> 8) & 0xFF),
				                     static_cast<char>((p >> 16) & 0xFF)};
				file.write(bgr, 3);
			}
		std::cout << "PC $" << std::hex << m._state->cpu.pc << std::dec << " after " << frames
				  << " frames of autostart\n";
		debug_state state;
		m.capture_debug(state);
		std::printf("A=%02X X=%02X Y=%02X SP=%02X P=%02X  $01=%02X  D011=%02X D016=%02X D018=%02X bank=%04X"
		            " raster=%d  CIA1 ICR mask %02X CRA %02X  CIA2 mask %02X\n",
		            state.a, state.x, state.y, state.sp, state.status, state.port, state.ctrl1, state.ctrl2,
		            state.mem_ptr, state.vic_bank, state.raster, state.cia_mask[0], state.timer_ctrl[0],
		            state.cia_mask[1]);
		for (int i = 0; i < state.disasm_count; ++i)
			std::printf("%s $%04X  %s\n", i == state.current ? ">" : " ", state.disasm[i].address,
			            state.disasm[i].text);
		print_screen(m);
		return file ? 0 : 1;
	}

	uint16_t parse_hex(const char* text)
	{
		return static_cast<uint16_t>(std::stoul(text, nullptr, 16));
	}

	// Assemble a 6502 source file and print the resulting machine code as a hex
	// dump (or the error with its line number). Drives src/assembler.cpp.
	int run_asm(const std::filesystem::path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			std::cerr << "Unable to read source file: " << path.string() << '\n';
			return 1;
		}
		const std::string source(bytes.begin(), bytes.end());
		const auto result = assemble(source);
		if (!result.ok)
		{
			std::cerr << path.filename().string() << ':' << result.error_line << ": error: " << result.error << '\n';
			return 1;
		}

		std::cout << "Assembled " << result.bytes.size() << " byte(s) at $" << std::hex << std::uppercase
				  << std::setw(4) << std::setfill('0') << result.origin << ":\n";
		for (size_t i = 0; i < result.bytes.size(); i += 16)
		{
			std::cout << '$' << std::setw(4) << std::setfill('0') << (result.origin + static_cast<unsigned>(i)) << "  ";
			for (size_t j = 0; j < 16 && i + j < result.bytes.size(); ++j)
				std::cout << std::setw(2) << std::setfill('0') << static_cast<unsigned>(result.bytes[i + j]) << ' ';
			std::cout << '\n';
		}
		std::cout << std::dec << std::nouppercase;
		return 0;
	}
}

int main(const int argc, char** argv)
{
#ifdef _MSC_VER
	// A failed CRT or STL debug assertion otherwise opens a modal dialog, which
	// hangs CTest and CI until it times out. Report to stderr and exit instead.
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

	// Unbuffered so an abort does not truncate the log: the last line printed is
	// then genuinely the last test reached.
	std::cout << std::unitbuf;

	if (argc == 3 && std::string_view(argv[1]) == "--list-disk") return list_disk(argv[2]) ? 0 : 1;

	// Run a raw binary as a bare 6502 (e.g. Klaus Dormann's 6502 functional
	// test): --run-bin <file> <loadHex> <startHex> [expectedTrapHex]
	if (argc >= 5 && std::string_view(argv[1]) == "--run-bin")
	{
		const auto load = parse_hex(argv[3]);
		const auto start = parse_hex(argv[4]);
		const bool has_expected = argc >= 6;
		const auto expected = has_expected ? parse_hex(argv[5]) : uint16_t{0};
		return run_bin(argv[2], load, start, has_expected, expected);
	}

	// Load a .prg and print the resulting text screen (Lorenz / VICE tests):
	// --run-prg <file.prg>
	if (argc == 3 && std::string_view(argv[1]) == "--run-prg") return run_prg_dump(argv[2]);

	// Assemble a 6502 source file and print the machine code: --asm <file.asm>
	if (argc == 3 && std::string_view(argv[1]) == "--asm") return run_asm(argv[2]);

	// Mount a disk image and LOAD a file off it through the kernal, then print the
	// screen: --run-disk <image> [name]
	if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--run-disk")
		return run_disk(argv[2], argc == 4 ? argv[3] : "*");

	// Render a SID tune to a WAV: --render-sid <file.sid> <out.wav> [seconds] [song]
	if (argc >= 4 && argc <= 6 && std::string_view(argv[1]) == "--render-sid")
		return render_sid(argv[2], argv[3], argc >= 5 ? std::stod(argv[4]) : 30.0, argc == 6 ? std::stoi(argv[5]) : 0);

	// Start any C64 media, run it and save the picture: --snap <file> <out.bmp> [seconds] [name]
	if (argc >= 4 && argc <= 6 && std::string_view(argv[1]) == "--snap")
		return snap(argv[2], argv[3], argc >= 5 ? std::stod(argv[4]) : 10.0, argc == 6 ? argv[5] : "");

	if (argc != 1)
	{
		std::cerr << "Usage: lib64-tests\n"
				  << "       lib64-tests --list-disk <image.d64|.d71|.d81>\n"
				  << "       lib64-tests --run-bin <file> <loadHex> <startHex> [expectedTrapHex]\n"
				  << "       lib64-tests --run-prg <file.prg>\n"
				  << "       lib64-tests --run-disk <image.d64|.d71|.d81> [name]\n"
				  << "       lib64-tests --render-sid <file.sid> <out.wav> [seconds] [song]\n"
				  << "       lib64-tests --snap <file> <out.bmp> [seconds] [name]\n"
				  << "       lib64-tests --asm <file.asm>\n";
		return 2;
	}

	test_runner tests;
	const auto fixture_folder = std::filesystem::path(__FILE__).parent_path();

	// Headless 6502 CPU validation (opcodes, flags, addressing, cycle timing).
	run_cpu_tests(tests);

	// VIC-II graphics chip validation (modes, sprites, collision, raster IRQ).
	run_vic_tests(tests);

	// CIA timer validation (counting, reload, ICR, one-shot, cascade, IRQ).
	run_cia_tests(tests);

	// SID output quality: aliasing, filter pass band and range, levels, clocks.
	run_sid_audio_tests(tests);

	// Built-in 6502 assembler validation (encoding, directives, round-trip).
	run_assembler_tests(tests);

	machine booted;
	tests.check(run_until_screen_contains(booted, "READY.", boot_cycle_limit), "boots to READY prompt");

	// The full cold-start screen must show the BASIC banner and free-memory line,
	// not just the READY prompt.
	tests.check(screen_contains(booted, "COMMODORE 64 BASIC V2"), "shows BASIC startup banner");
	tests.check(screen_contains(booted, "38911 BASIC BYTES FREE"), "shows free memory message");

	// Regression: driving the periodic IRQ the way the GUI app does must not derail
	// the kernal (a mis-vectored IRQ used to unbalance the stack and wipe the
	// screen down to a bare READY prompt).
	run_app_style_ticks(booted, 200);
	tests.check(screen_contains(booted, "COMMODORE 64 BASIC V2") && screen_contains(booted, "READY."),
	            "keeps full screen after periodic IRQs");

	type_line(booted, "PRINT 6*7");
	tests.check(run_until_screen_contains(booted, "42", command_cycle_limit), "executes interactive BASIC");

	// Function keys inject their PETSCII codes into the keyboard buffer exactly
	// like normal keys; a running program reads them with GET. (C64 BASIC does
	// nothing visible with them at the direct-mode READY prompt.)
	{
		machine fk;
		tests.check(run_until_screen_contains(fk, "READY.", boot_cycle_limit), "boots for function-key test");
		type_line(fk, "10 GET A$:IF A$=\"\" THEN 10");
		type_line(fk, "20 PRINT ASC(A$)");
		type_line(fk, "RUN");
		fk.exec(1'000'000); // process RUN and settle into the GET loop
		fk.add_char(133);   // F1 -> PETSCII 133
		tests.check(run_until_screen_contains(fk, "133", command_cycle_limit),
		            "function key F1 reaches a running program");
	}

	// Keyboard matrix: holding a key in the CIA1 matrix must be picked up by the
	// kernal's IRQ-driven keyboard scan and typed. 'Q' is at row 7, column 6 and
	// does not appear in the boot banner.
	{
		machine kb;
		tests.check(run_until_screen_contains(kb, "READY.", boot_cycle_limit), "boots for keyboard-matrix test");
		kb.set_key(7, 6, true);                      // press Q
		for (int i = 0; i < 8; ++i) kb.exec(20'000); // hold across several keyboard scans
		kb.set_key(7, 6, false);                     // release
		tests.check(run_until_screen_contains(kb, "Q", command_cycle_limit),
		            "keyboard matrix types through the kernal scan");
	}

	// A host's key press and release can both arrive between two exec() calls
	// (a frame is emulated in a burst of about a millisecond). queue_key holds
	// each change for a whole exec(), so instant taps - a doubled letter and a
	// shifted key among them - type intact, and nothing is left held.
	{
		constexpr int frame = 63 * 312;
		machine taps;
		tests.check(run_until_screen_contains(taps, "READY.", boot_cycle_limit), "boots for key-tap test");
		const auto tap = [&](const int row, const int col)
		{
			taps.queue_key(row, col, true);
			taps.queue_key(row, col, false);
		};
		tap(5, 2);                  // L
		tap(5, 2);                  // L
		tap(4, 6);                  // O
		taps.queue_key(1, 7, true); // SHIFT + 2 is a quote
		tap(7, 3);
		taps.queue_key(1, 7, false);
		for (int i = 0; i < 25; ++i) taps.exec(frame);
		tests.check(screen_contains(taps, "LLO\""), "queued key taps shorter than a frame all type");
		tests.check(std::all_of(std::begin(taps._state->key_matrix), std::end(taps._state->key_matrix),
		                        [](const uint8_t row) { return row == 0xFF; }),
		            "queued key taps leave no key held");

		taps.queue_key(7, 6, true); // Q, dropped before any exec() can apply it
		taps.clear_keys();
		for (int i = 0; i < 25; ++i) taps.exec(frame);
		tests.check(!screen_contains(taps, "Q"), "clear_keys drops queued key changes");
	}

	// RESTORE pulses NMI. Alone, the KERNAL's handler just returns; with RUN/STOP
	// held it warm-starts: the screen is cleared and the VIC set up again, so the
	// black border the program asked for goes back to light blue.
	{
		constexpr int frame = 63 * 312;
		machine nmi;
		tests.check(run_until_screen_contains(nmi, "READY.", boot_cycle_limit), "boots for RESTORE test");
		type_line(nmi, "POKE 53280,0");
		for (int i = 0; i < 25; ++i) nmi.exec(frame);
		const auto border = [&]
		{
			return io(nmi, 0xD020) & 0x0F;
		};

		nmi.press_restore();
		for (int i = 0; i < 25; ++i) nmi.exec(frame);
		tests.check(border() == 0 && screen_contains(nmi, "POKE"), "RESTORE alone leaves the program alone");

		nmi.queue_key(7, 7, true); // RUN/STOP, held as RESTORE goes down
		nmi.press_restore();
		nmi.queue_key(7, 7, false);
		for (int i = 0; i < 25; ++i) nmi.exec(frame);
		tests.check(border() == 14 && !screen_contains(nmi, "POKE") && screen_contains(nmi, "READY."),
		            "RUN/STOP+RESTORE warm-starts BASIC");
	}

	// VIC-II frame buffer rendering: boot with rendering on and confirm the
	// renderer produces a real picture (border around a background screen with
	// text pixels), not a blank buffer.
	{
		machine video;
		video.set_render_enabled(true);
		const bool booted_video = run_until_screen_contains(video, "READY.", boot_cycle_limit);
		video.exec(60'000); // let a couple of frames of scanlines render
		tests.check(booted_video, "boots with video rendering enabled");

		const auto* fb = video.framebuffer();
		const auto width = machine::framebuffer_width();
		const auto height = machine::framebuffer_height();
		const auto border = vic_palette[io(video, 0xD020) & 0x0F];
		const auto background = vic_palette[io(video, 0xD021) & 0x0F];

		constexpr int border_x = (384 - 320) / 2;
		constexpr int border_y = (272 - 200) / 2;

		bool has_background = false;
		bool has_text = false;
		for (int y = border_y; y < border_y + 200; ++y)
		{
			for (int x = border_x; x < border_x + 320; ++x)
			{
				const auto pixel = fb[y * width + x];
				if (pixel == background) has_background = true;
				else has_text = true;
			}
		}

		tests.check(fb[0] == border, "renders the border colour");
		tests.check(has_background, "renders the screen background");
		tests.check(has_text, "renders text pixels over the background");
		(void)height;
	}

	// Sprite collision registers ($D01E sprite-sprite, $D01F sprite-background)
	// must clear when read; otherwise a stale collision persists every frame and
	// games (e.g. Pitfall) think the player keeps colliding.
	{
		machine collide;
		io(collide, 0xD01E) = 0x03;
		io(collide, 0xD01F) = 0x81;
		const auto ss_first = collide._state->ram_read(0xD01E);
		const auto ss_again = collide._state->ram_read(0xD01E);
		const auto sb_first = collide._state->ram_read(0xD01F);
		const auto sb_again = collide._state->ram_read(0xD01F);
		tests.check(ss_first == 0x03 && ss_again == 0x00, "sprite-sprite collision clears on read");
		tests.check(sb_first == 0x81 && sb_again == 0x00, "sprite-background collision clears on read");
	}

	machine prg_machine;
	tests.check(run_until_screen_contains(prg_machine, "READY.", boot_cycle_limit), "boots before loading PRG");
	const auto prg = read_file(fixture_folder / "example.prg");
	const auto loaded = prg_machine.load_prg(prg.data(), prg.size());
	tests.check(loaded, "loads example.prg");
	tests.check(run_until_screen_contains(prg_machine, "HELLO WORLD", command_cycle_limit), "runs example.prg");

	// is_prg is load_prg's own acceptance test, so a host can reject a file before
	// it resets the machine to load it.
	{
		const uint8_t address_only[] = {0x01, 0x08};
		const uint8_t fits_exactly[] = {0xFF, 0xFF, 0x60};
		const uint8_t runs_past_end[] = {0xFF, 0xFF, 0x60, 0x60};
		tests.check(machine::is_prg(prg.data(), prg.size()), "is_prg accepts example.prg");
		tests.check(!machine::is_prg(address_only, sizeof(address_only)), "is_prg rejects a load address with no data");
		tests.check(machine::is_prg(fits_exactly, sizeof(fits_exactly)) &&
		                !machine::is_prg(runs_past_end, sizeof(runs_past_end)),
		            "is_prg rejects only a program that runs past $FFFF");
		tests.check(!prg_machine.load_prg(runs_past_end, sizeof(runs_past_end)),
		            "load_prg rejects what is_prg rejects");
	}

	for (const auto name : {"gng_bl.crt", "pitfall.crt", "river_raid.crt"})
	{
		const auto crt = read_file(fixture_folder / name);
		const auto info = inspect_crt(crt);
		tests.check(info.valid, std::string("parses CRT fixture: ") + name);
		if (info.valid)
			std::cout << "     type=" << info.hardware_type << ", chips=" << info.chip_count
					  << ", ROM bytes=" << info.rom_size << '\n';
	}

	// Load the normal (type 0) cartridges into a real machine and confirm the
	// cartridge ROM actually appears in the CPU address space via banking.
	for (const auto name : {"pitfall.crt", "river_raid.crt"})
	{
		const auto crt = read_file(fixture_folder / name);
		machine cart_machine;
		const bool cart_loaded = cart_machine.load_crt(crt.data(), crt.size());
		tests.check(cart_loaded, std::string("loads cartridge into machine: ") + name);
		if (cart_loaded)
		{
			const auto header_len = read_big_endian_32(crt, 0x10);
			const auto load_addr = read_big_endian_16(crt, header_len + 12);
			const auto* rom = crt.data() + header_len + 16;
			bool maps = true;
			for (int i = 0; i < 16; ++i)
			{
				if (cart_machine._state->ram_read(static_cast<uint16_t>(load_addr + i)) != rom[i])
				{
					maps = false;
					break;
				}
			}
			tests.check(maps, std::string("maps cartridge ROM into memory: ") + name);
		}
	}

	// The PLA only shows cartridge ROM while the processor port asks for ROM, so a
	// cartridge can reach the RAM underneath itself by writing $34 to $01.
	{
		const auto crt = read_file(fixture_folder / "pitfall.crt");
		machine cart_machine;
		if (cart_machine.load_crt(crt.data(), crt.size()))
		{
			const auto rom_byte = cart_machine._state->ram_read(0x8000);
			cart_machine._state->ram_write(0x8000, static_cast<uint8_t>(rom_byte ^ 0xFF));
			cart_machine._state->ram_write(0x0001, 0x34); // all RAM, no BASIC/KERNAL/ROML
			const auto ram_byte = cart_machine._state->ram_read(0x8000);
			cart_machine._state->ram_write(0x0001, 0x37);
			tests.check(ram_byte == static_cast<uint8_t>(rom_byte ^ 0xFF),
			            "banking cartridge ROM out reveals the RAM underneath");
			tests.check(cart_machine._state->ram_read(0x8000) == rom_byte, "banking cartridge ROM back in restores it");
		}
	}

	// A malformed image is rejected without side effects: the inserted cartridge
	// stays mapped, so the program running from it keeps running.
	{
		const auto crt = read_file(fixture_folder / "pitfall.crt");
		machine cart_machine;
		if (cart_machine.load_crt(crt.data(), crt.size()))
		{
			const auto rom_byte = cart_machine._state->ram_read(0x8000);
			const auto pc = cart_machine._state->cpu.pc;
			auto truncated = crt;
			truncated.resize(0x40); // a valid header, but no CHIP packets
			tests.check(!cart_machine.load_crt(truncated.data(), truncated.size()), "a CRT with no ROM is rejected");
			tests.check(cart_machine._state->ram_read(0x8000) == rom_byte && cart_machine._state->cpu.pc == pc,
			            "a rejected CRT leaves the inserted cartridge running");
		}
	}

	// A 4K Ultimax cartridge at $F000 repeats through $E000-$FFFF, which is
	// where its reset vector comes from; the VIC sees its top 4K at $3000.
	{
		std::vector<uint8_t> crt(0x40, 0);
		std::memcpy(crt.data(), "C64 CARTRIDGE   ", 16);
		crt[0x13] = 0x40; // header length
		crt[0x14] = 0x01; // version 1.0
		crt[0x18] = 1;    // /EXROM high
		crt[0x19] = 0;    // /GAME low: Ultimax
		const uint8_t chip[16] = {'C', 'H', 'I', 'P', 0, 0, 0x10, 0x10, 0, 0, 0, 0, 0xF0, 0x00, 0x10, 0x00};
		crt.insert(crt.end(), std::begin(chip), std::end(chip));
		std::vector<uint8_t> rom(0x1000, 0xEA);
		rom[0x0000] = 0x4C;
		rom[0x0001] = 0x00;
		rom[0x0002] = 0xF0; // JMP $F000
		rom[0x0FFC] = 0x00;
		rom[0x0FFD] = 0xF0; // reset vector $F000
		rom[0x0123] = 0x5A;
		crt.insert(crt.end(), rom.begin(), rom.end());

		machine ultimax;
		const bool ultimax_loaded = ultimax.load_crt(crt.data(), crt.size());
		tests.check(ultimax_loaded && ultimax._state->cpu.pc == 0xF000,
		            "a 4K Ultimax cartridge supplies the reset vector");
		tests.check(ultimax._state->ram_read(0xE123) == 0x5A && ultimax._state->ram_read(0xF123) == 0x5A,
		            "a 4K Ultimax ROM repeats through $E000-$FFFF");
		uint8_t vic_byte = 0;
		tests.check(ultimax._state->cart.vic_read(0x3123, vic_byte) && vic_byte == 0x5A,
		            "the VIC sees an Ultimax cartridge's ROM at $3000");
	}

	// Bank switching, through the bus as a program drives it, for the hardware
	// types whose handlers are easiest to get wrong. Each chip is filled with a
	// byte of its own, so a read says which chip is mapped.
	{
		struct test_chip
		{
			uint16_t bank;
			uint16_t load;
			uint16_t size;
			uint8_t fill;
		};
		const auto make_crt =
			[](const int type, const uint8_t exrom, const uint8_t game, const std::initializer_list<test_chip> chips)
		{
			std::vector<uint8_t> crt(0x40, 0);
			std::memcpy(crt.data(), "C64 CARTRIDGE   ", 16);
			crt[0x13] = 0x40; // header length
			crt[0x14] = 0x01; // version 1.0
			crt[0x16] = static_cast<uint8_t>(type >> 8);
			crt[0x17] = static_cast<uint8_t>(type);
			crt[0x18] = exrom;
			crt[0x19] = game;
			for (const auto& chip : chips)
			{
				const uint32_t total = 0x10u + chip.size;
				// clang-format off
				const uint8_t header[16] = {'C', 'H', 'I', 'P',
					static_cast<uint8_t>(total >> 24), static_cast<uint8_t>(total >> 16),
					static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total), 0, 0,
					static_cast<uint8_t>(chip.bank >> 8), static_cast<uint8_t>(chip.bank),
					static_cast<uint8_t>(chip.load >> 8), static_cast<uint8_t>(chip.load),
					static_cast<uint8_t>(chip.size >> 8), static_cast<uint8_t>(chip.size)};
				// clang-format on
				crt.insert(crt.end(), std::begin(header), std::end(header));
				crt.resize(crt.size() + chip.size, chip.fill);
			}
			return crt;
		};
		const uint8_t basic_first = rom_basic[0];

		// EasyFlash boots in Ultimax mode from ROMH; $DE02 then sets /EXROM from
		// bit 1, and /GAME from bit 0 while bit 2 is set, the boot jumper's low
		// otherwise. $DE00 picks the bank, and 256 bytes of RAM sit at $DF00.
		{
			const auto crt = make_crt(32, 1, 0,
			                          {{0, 0x8000, 0x2000, 0x10},
			                           {0, 0xA000, 0x2000, 0x20},
			                           {1, 0x8000, 0x2000, 0x11},
			                           {1, 0xA000, 0x2000, 0x21}});
			machine m;
			auto& s = *m._state;
			tests.check(m.load_crt(crt.data(), crt.size()) && s.ram_read(0xE000) == 0x20,
			            "EasyFlash boots in Ultimax mode, from ROMH");
			s.ram_write(0xDE02, 0x07);
			tests.check(s.ram_read(0x8000) == 0x10 && s.ram_read(0xA000) == 0x20,
			            "EasyFlash's $DE02 = 7 selects 16K mode");
			s.ram_write(0xDE00, 0x01);
			tests.check(s.ram_read(0x8000) == 0x11 && s.ram_read(0xA000) == 0x21,
			            "EasyFlash's $DE00 switches both chips to another bank");
			s.ram_write(0xDE02, 0x06);
			tests.check(s.ram_read(0x8000) == 0x11 && s.ram_read(0xA000) == basic_first,
			            "EasyFlash's $DE02 = 6 releases /GAME: 8K mode");
			s.ram_write(0xDE02, 0x00);
			tests.check(s.ram_read(0xE000) == 0x21, "without bit 2 the boot jumper holds /GAME low: Ultimax again");
			s.ram_write(0xDF05, 0x77);
			tests.check(s.ram_read(0xDF05) == 0x77, "EasyFlash's RAM at $DF00 keeps what is written");
		}

		// Simons' BASIC: reading IO1 turns ROMH off (8K), writing it turns it on.
		{
			const auto crt = make_crt(4, 0, 0, {{0, 0x8000, 0x2000, 0x30}, {0, 0xA000, 0x2000, 0x31}});
			machine m;
			auto& s = *m._state;
			tests.check(m.load_crt(crt.data(), crt.size()) && s.ram_read(0xA000) == 0x31,
			            "Simons' BASIC starts in 16K mode");
			(void)s.ram_read(0xDE00);
			tests.check(s.ram_read(0x8000) == 0x30 && s.ram_read(0xA000) == basic_first,
			            "reading IO1 drops Simons' BASIC to 8K, showing BASIC");
			s.ram_write(0xDE00, 0);
			tests.check(s.ram_read(0xA000) == 0x31, "writing IO1 brings its ROMH back");
		}

		// Westermann: reading IO2 drops the cartridge from 16K to 8K.
		{
			const auto crt = make_crt(11, 0, 0, {{0, 0x8000, 0x2000, 0x40}, {0, 0xA000, 0x2000, 0x41}});
			machine m;
			auto& s = *m._state;
			tests.check(m.load_crt(crt.data(), crt.size()) && s.ram_read(0xA000) == 0x41,
			            "a Westermann cartridge starts in 16K mode");
			(void)s.ram_read(0xDF00);
			tests.check(s.ram_read(0x8000) == 0x40 && s.ram_read(0xA000) == basic_first, "reading IO2 drops it to 8K");
		}

		// Zaxxon: a 4K ROML repeated through $8000-$9FFF; reading its upper copy
		// selects ROMH's second bank, its lower copy the first.
		{
			const auto crt = make_crt(18, 0, 0,
			                          {{0, 0x8000, 0x1000, 0x50},
			                           {0, 0xA000, 0x2000, 0x60},
			                           {1, 0xA000, 0x2000, 0x61}});
			machine m;
			auto& s = *m._state;
			tests.check(m.load_crt(crt.data(), crt.size()) && s.ram_read(0xA000) == 0x60,
			            "Zaxxon starts on its first ROMH bank");
			tests.check(s.ram_read(0x9000) == 0x50 && s.ram_read(0xA000) == 0x61,
			            "reading ROML's upper 4K selects the second ROMH bank");
			tests.check(s.ram_read(0x8000) == 0x50 && s.ram_read(0xA000) == 0x60,
			            "reading its lower 4K selects the first again");
		}

		// RGCD: $DE00 bits 0-2 pick an 8K bank; bit 3 switches the cartridge off.
		{
			const auto crt = make_crt(57, 0, 1, {{0, 0x8000, 0x2000, 0x70}, {1, 0x8000, 0x2000, 0x71}});
			machine m;
			auto& s = *m._state;
			tests.check(m.load_crt(crt.data(), crt.size()) && s.ram_read(0x8000) == 0x70,
			            "an RGCD cartridge starts on bank 0");
			s.ram_write(0xDE00, 0x01);
			tests.check(s.ram_read(0x8000) == 0x71, "RGCD's $DE00 picks another bank");
			s.ram_write(0xDE00, 0x08);
			const auto off = s.ram_read(0x8000);
			tests.check(off != 0x70 && off != 0x71, "RGCD's bit 3 switches the cartridge off");
		}
	}

	// I/O registers have storage of their own: a program can keep data in the
	// RAM under $D000-$DFFF without touching the chips, and read it back.
	{
		machine m;
		auto& s = *m._state;
		s.ram_write(0xD020, 0x05); // border green, I/O banked in
		s.ram_write(0x0001, 0x34); // all RAM
		s.ram_write(0xD020, 0xAA);
		s.ram_write(0xD800, 0x55);
		s.ram_write(0xDC0E, 0x01);
		const uint8_t under = s.ram_read(0xD020);
		s.ram_write(0x0001, 0x37); // I/O back in
		tests.check(under == 0xAA && s.RAM[0xD800] == 0x55, "RAM under I/O keeps what is written there");
		tests.check((s.ram_read(0xD020) & 0x0F) == 0x05 && !s.cia1.timer_a_running,
		            "writes to the RAM under I/O leave the chips alone");
		s.ram_write(0xD800, 0x0C);
		tests.check(s.RAM[0xD800] == 0x55 && (s.ram_read(0xD800) & 0x0F) == 0x0C,
		            "colour RAM is separate from the RAM beneath it");
	}

	// An externally raised IRQ (machine::irq, the app's 50 Hz tick) is a source in
	// its own right: recomputing the interrupt line must not discard it.
	{
		machine m;
		m._state->cpu.clear_interrupt();
		m.irq();
		m._state->read_cia1(0xDC0D); // reading the ICR recomputes the IRQ line
		tests.check(m._state->irq_pending, "an external IRQ survives a CIA register read");
		m.exec(1);
		tests.check(!m._state->ext_irq && (m._state->cpu.status & FLAG_INTERRUPT) != 0,
		            "an external IRQ is taken once and then released");
	}

	// The disassembler and the CPU share one length table; if they disagree, a
	// listing that contains an undocumented opcode desynchronises from the code
	// that actually runs.
	{
		bool all_match = true;
		for (int op = 0; op < 256; ++op)
		{
			if (std::strcmp(opcodes[op].name, "UNDEFINED") != 0) continue;
			if (opcodes[op].length == 0) continue; // JAM: the CPU deliberately spins
			machine m;
			m._state->raw_ram = true;
			m._state->cpu.pc = 0x1000;
			m._state->RAM[0x1000] = static_cast<uint8_t>(op);
			m.exec(1);
			if (m._state->cpu.pc != 0x1000 + opcodes[op].length) all_match = false;
		}
		tests.check(all_match, "undocumented opcodes advance the PC by their table length");
	}

	// SID audio: boot, program voice 1 with a gated sawtooth at full volume and
	// confirm the SID pipeline renders non-silent 16-bit PCM.
	{
		machine audio;
		run_until_screen_contains(audio, "READY.", boot_cycle_limit);
		audio.set_audio_enabled(true); // sync the audio clock to the post-boot cycle

		auto& sid = audio._state->sid;
		sid.write_now(0x18, 0x0F); // master volume max, no filter
		sid.write_now(0x00, 0x00); // voice 1 frequency low
		sid.write_now(0x01, 0x20); // voice 1 frequency high
		sid.write_now(0x05, 0x00); // attack=0, decay=0
		sid.write_now(0x06, 0xF0); // sustain=15, release=0
		sid.write_now(0x04, 0x21); // sawtooth waveform + gate on

		audio.exec(20'000); // ~one frame of cycles

		int16_t buffer[2048];
		const int samples = audio.generate_audio(buffer, 2048);
		bool non_silent = false;
		for (int i = 0; i < samples; ++i)
			if (buffer[i] != 0)
			{
				non_silent = true;
				break;
			}

		tests.check(samples > 0, "SID renders audio samples");
		tests.check(non_silent, "SID output is non-silent");
	}

	// SID tune playback: load a real .sid file, run the installed player driver
	// for a few frames and confirm the play routine drives the SID to audible
	// output.
	{
		const auto tune_bytes = read_file(fixture_folder / "cybernoid.sid");
		machine tune;
		const bool tune_loaded = tune.load_sid(tune_bytes.data(), tune_bytes.size());
		tests.check(tune_loaded, "loads cybernoid.sid tune");

		bool non_silent = false;
		int16_t buffer[2048];
		for (int frame = 0; frame < 100 && !non_silent; ++frame)
		{
			tune.exec(20'000);
			const int samples = tune.generate_audio(buffer, 2048);
			for (int i = 0; i < samples; ++i)
				if (buffer[i] != 0)
				{
					non_silent = true;
					break;
				}
		}
		tests.check(non_silent, "SID tune drives audible output");
	}

	// SID player front end: the tune's metadata and sub-tune list are painted on
	// the emulated screen, the number keys switch sub-tune and X stops playback.
	{
		const auto tune_bytes = read_file(fixture_folder / "cybernoid.sid");
		machine tune;
		tune.load_sid(tune_bytes.data(), tune_bytes.size());
		tune.exec(20'000);

		const sid_player_state& player = tune._state->sid_player;
		auto screen = [&]
		{
			return tune._state->RAM + player.screen_base;
		};

		// Screen codes back to ASCII: letters are 1-26, and space through '?'
		// share their ASCII values. Bit 7 is the reverse-video flag.
		auto row_text = [&](int y)
		{
			std::string out;
			for (int x = 0; x < 40; ++x)
			{
				const uint8_t code = screen()[y * 40 + x] & 0x7F;
				if (code >= 1 && code <= 26) out += static_cast<char>('A' + code - 1);
				else if (code >= 0x20 && code <= 0x3F) out += static_cast<char>(code);
				else out += '.';
			}
			return out;
		};
		auto reversed = [&](int x, int y)
		{
			return (screen()[y * 40 + x] & 0x80) != 0;
		};

		tests.check(row_text(1).find("SID PLAYER") != std::string::npos, "SID player draws its title");
		tests.check(row_text(3).find("CYBERNOID") != std::string::npos, "SID player shows the tune name");
		tests.check(row_text(4).find("JEROEN TEL") != std::string::npos, "SID player shows the author");
		tests.check(player.songs == 2, "SID player reads the sub-tune count");
		tests.check(reversed(11, 10) && !reversed(14, 10), "SID player highlights the playing track");

		// "2" (matrix row 7, column 3) selects the second sub-tune.
		tune.set_key(7, 3, true);
		tune.exec(20'000);
		tune.set_key(7, 3, false);
		tune.exec(20'000);
		tests.check(tune.sid_song() == 2, "a number key selects a sub-tune");
		tests.check(reversed(14, 10) && !reversed(11, 10), "the highlight follows the selection");

		// "X" (matrix row 2, column 7) stops the tune and leaves a bare machine.
		tune.set_key(2, 7, true);
		tune.exec(20'000);
		tests.check(!tune.sid_player_active(), "X stops the SID player");
	}

	// A tune's header read without loading it - what a host lists or indexes a
	// tune by - and the same validation load_sid() applies.
	{
		sid_tune_info info;
		const auto cybernoid = read_file(fixture_folder / "cybernoid.sid");
		tests.check(machine::parse_sid(cybernoid.data(), cybernoid.size(), info), "parse_sid reads a PSID header");
		tests.check(std::string_view(info.title) == "Cybernoid" && std::string_view(info.author) == "Jeroen Tel" &&
		                std::string_view(info.released) == "1988 Hewson",
		            "parse_sid reads the title, author and release");
		tests.check(!info.rsid && info.version == 2 && info.songs == 2 && info.start_song == 1,
		            "parse_sid reads the format, sub-tune count and start tune");
		tests.check(info.load_address == 0xAE00 && info.init_address == 0xAE00 && info.play_address == 0xAE06,
		            "parse_sid resolves the embedded load address and a zero init address");
		tests.check(!info.chip_8580 && !info.ntsc, "parse_sid reads the 6581 and PAL flags");

		sid_tune_info ninja;
		const auto last_ninja = read_file(fixture_folder / "last_ninja.sid");
		tests.check(machine::parse_sid(last_ninja.data(), last_ninja.size(), ninja) && ninja.songs == 11 &&
		                ninja.start_song == 3 && ninja.load_address == 0x2000 && ninja.init_address == 0x2003 &&
		                ninja.play_address == 0x2000,
		            "parse_sid reads a tune that starts on a later sub-tune");

		sid_tune_info giana;
		const auto ggs = read_file(fixture_folder / "ggs.sid");
		tests.check(machine::parse_sid(ggs.data(), ggs.size(), giana) && giana.rsid && giana.songs == 23,
		            "parse_sid reads an RSID header");

		std::vector<uint8_t> broken = cybernoid;
		broken[0] = 'X';
		sid_tune_info rejected;
		machine untouched;
		tests.check(!machine::parse_sid(broken.data(), broken.size(), rejected) &&
		                !untouched.load_sid(broken.data(), broken.size()),
		            "parse_sid and load_sid both refuse a bad magic number");
		tests.check(!machine::parse_sid(cybernoid.data(), 0x7B, rejected), "parse_sid refuses a truncated header");
	}

	// The SID monitor hooks: register writes are counted, envelopes are live while
	// audio runs, and a muted voice leaves the mix but keeps running.
	{
		const auto tune_bytes = read_file(fixture_folder / "cybernoid.sid");
		auto render = [&](machine& m, const int frames)
		{
			std::vector<int16_t> pcm;
			int16_t buffer[4096];
			for (int frame = 0; frame < frames; ++frame)
			{
				m.exec(pal_cycles_per_frame);
				const int samples = m.generate_audio(buffer, 4096);
				pcm.insert(pcm.end(), buffer, buffer + samples);
			}
			return pcm;
		};

		machine plain;
		plain.load_sid(tune_bytes.data(), tune_bytes.size());
		sid_snapshot before;
		plain.capture_sid(before);
		const auto plain_pcm = render(plain, 100);
		sid_snapshot after;
		plain.capture_sid(after);

		uint32_t writes = 0;
		bool envelope_live = false;
		for (int r = 0; r < 25; ++r) writes += after.writes[r] - before.writes[r];
		for (int v = 0; v < 3; ++v) envelope_live = envelope_live || after.envelope[v] != 0;
		tests.check(writes > 100, "capture_sid counts the play routine's register writes");
		tests.check(envelope_live, "capture_sid reports a sounding voice's envelope level");
		tests.check(std::equal(after.regs, after.regs + 32, plain._state->sid_regs),
		            "capture_sid copies the register file");

		machine muted;
		muted.load_sid(tune_bytes.data(), tune_bytes.size());
		muted.set_sid_voice_muted(0, true);
		tests.check(muted.sid_voice_muted(0) && !muted.sid_voice_muted(1), "a muted voice reports as muted");
		const auto muted_pcm = render(muted, 100);
		tests.check(muted_pcm.size() == plain_pcm.size() && muted_pcm != plain_pcm,
		            "muting a voice changes the output");

		machine silent;
		silent.load_sid(tune_bytes.data(), tune_bytes.size());
		for (int v = 0; v < 3; ++v) silent.set_sid_voice_muted(v, true);
		const auto silent_pcm = render(silent, 100);
		int peak = 0, plain_peak = 0;
		for (size_t i = silent_pcm.size() / 2; i < silent_pcm.size(); ++i)
			peak = std::max(peak, std::abs(static_cast<int>(silent_pcm[i])));
		for (size_t i = plain_pcm.size() / 2; i < plain_pcm.size(); ++i)
			plain_peak = std::max(plain_peak, std::abs(static_cast<int>(plain_pcm[i])));
		tests.check(peak * 20 < plain_peak, "muting every voice silences the tune");

		silent.sid_select_song(2);
		tests.check(silent.sid_voice_muted(2), "a mute survives a change of sub-tune");
		silent.set_sid_voice_muted(2, false);
		tests.check(!silent.sid_voice_muted(2), "a voice can be unmuted");
	}

	// reset() is a cold start: the KERNAL boots to READY. again and a tune stops.
	{
		const auto tune_bytes = read_file(fixture_folder / "cybernoid.sid");
		machine m;
		m.load_sid(tune_bytes.data(), tune_bytes.size());
		m.exec(pal_cycles_per_frame);
		m.reset();
		tests.check(!m.sid_player_active(), "reset leaves a playing tune");
		tests.check(run_until_screen_contains(m, "READY.", boot_cycle_limit), "reset boots the KERNAL to READY.");
	}

	// Debug snapshot: every addressing mode formats the way a 6502 monitor
	// prints it, and a capture of a running machine lines the disassembly window
	// up on the PC without disturbing any I/O register.
	{
		// clang-format off
		struct { uint8_t bytes[3]; const char* text; } cases[] = {
			{{0xEA, 0x00, 0x00}, "NOP"},
			{{0x0A, 0x00, 0x00}, "ASL A"},
			{{0xA9, 0x42, 0x00}, "LDA #$42"},
			{{0xA5, 0xFB, 0x00}, "LDA $FB"},
			{{0xB5, 0xFB, 0x00}, "LDA $FB,X"},
			{{0xAD, 0x34, 0x12}, "LDA $1234"},
			{{0xBD, 0x34, 0x12}, "LDA $1234,X"},
			{{0xB9, 0x34, 0x12}, "LDA $1234,Y"},
			{{0x6C, 0x14, 0x03}, "JMP ($0314)"},
			{{0xA1, 0xFB, 0x00}, "LDA ($FB,X)"},
			{{0xB1, 0xFB, 0x00}, "LDA ($FB),Y"},
			{{0xD0, 0xFE, 0x00}, "BNE $1000"}, // relative, from $1000
		};
		// clang-format on

		bool all_match = true;
		for (const auto& c : cases)
		{
			char text[20];
			disassemble_6502(0x1000, c.bytes, text, sizeof(text));
			if (std::strcmp(text, c.text) != 0) all_match = false;
		}
		tests.check(all_match, "disassembles every addressing mode");

		machine booted_debug;
		booted_debug.exec(3'000'000);
		const uint8_t before = booted_debug._state->cia1.icr_data;

		debug_state snapshot;
		booted_debug.capture_debug(snapshot);

		tests.check(snapshot.pc == booted_debug._state->cpu.pc, "debug snapshot reports the PC");
		tests.check(snapshot.disasm_count == DEBUG_DISASM_LINES &&
		                snapshot.disasm[snapshot.current].address == snapshot.pc,
		            "disassembly window lands on the PC");
		tests.check(booted_debug._state->cia1.icr_data == before, "debug snapshot leaves latched I/O untouched");
	}

	disk_directory disk;
	const auto listed = list_disk(fixture_folder / "1942.d64", &disk);
	tests.check(listed, "lists 1942.d64 directory");
	tests.check(disk.format == disk_image_format::d64, "detects D64 format");
	tests.check(disk.name == "1986 ELITE", "reads D64 disk name");
	tests.check(has_entry_starting_with(disk, "1942"), "finds 1942 program on disk");
	tests.check(recognizes_empty_disk(349696, 91392, disk_image_format::d71), "detects D71 format");
	tests.check(recognizes_empty_disk(819200, 399360, disk_image_format::d81), "detects D81 format");

	// Extract the first program off the disk (as LOAD"*",8 would) and by name.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		std::vector<uint8_t> first;
		const bool got_first = read_disk_file(d64.data(), d64.size(), "*", first);
		tests.check(got_first && first.size() > 2, "extracts first program from 1942.d64");

		std::vector<uint8_t> by_name;
		read_disk_file(d64.data(), d64.size(), "1942*", by_name);
		tests.check(!by_name.empty() && by_name == first, "extracts disk file by name pattern");

		// CBM DOS wildcards: '?' matches one character, '*' the rest of the name.
		std::vector<uint8_t> by_glob;
		read_disk_file(d64.data(), d64.size(), "19?2 D*", by_glob);
		tests.check(!by_glob.empty() && by_glob != first, "matches a ? wildcard mid-pattern");
		tests.check(disk.free_blocks > 0 && disk.free_blocks < 664, "counts free blocks from the BAM");
	}

	// --- Device 8: a mounted image services the kernal LOAD routine ----------
	// This is what makes a disk-based game work: its loader keeps LOADing files
	// long after the first program started.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");

		machine drive;
		tests.check(drive.insert_disk(d64.data(), d64.size()), "inserts a disk image into device 8");
		tests.check(drive.has_disk(), "reports the drive as loaded");
		tests.check(!drive.insert_disk(prg.data(), prg.size()), "rejects a non-disk image");
		drive.eject_disk();
		tests.check(!drive.has_disk(), "reports the drive as empty after eject");
	}

	// A machine-code routine calling SETNAM/SETLFS/LOAD must get the documented
	// results back: carry clear, X/Y = end address + 1, and the file in RAM.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		std::vector<uint8_t> expected;
		read_disk_file(d64.data(), d64.size(), "1942 D*", expected);

		machine drive;
		drive.insert_disk(d64.data(), d64.size());
		tests.check(run_until_screen_contains(drive, "READY.", boot_cycle_limit), "boots with a disk inserted");

		constexpr std::string_view name = "1942 D*";
		for (size_t i = 0; i < name.size(); ++i) drive._state->RAM[0xC100 + i] = static_cast<uint8_t>(name[i]);

		// clang-format off
		const uint8_t routine[] = {
			0xA9, static_cast<uint8_t>(name.size()), // LDA #len
			0xA2, 0x00, 0xA0, 0xC1,                  // LDX/LDY = $C100
			0x20, 0xBD, 0xFF,                        // JSR SETNAM
			0xA9, 0x01, 0xA2, 0x08, 0xA0, 0x01,      // file 1, device 8, secondary 1
			0x20, 0xBA, 0xFF,                        // JSR SETLFS
			0xA9, 0x00,                              // LDA #0 (load, not verify)
			0x20, 0xD5, 0xFF,                        // JSR LOAD
			0x08,                                    // PHP
			0x86, 0xFB, 0x84, 0xFC,                  // STX $FB / STY $FC (end address)
			0x68, 0x85, 0xFD,                        // PLA / STA $FD (status flags)
			0x60,                                    // RTS
		};
		// clang-format on
		std::copy(std::begin(routine), std::end(routine), drive._state->RAM + 0xC000);

		type_line(drive, "SYS 49152");
		drive.exec(command_cycle_limit);

		const auto load_addr = static_cast<uint16_t>(expected[0] | expected[1] << 8);
		const auto end = static_cast<uint16_t>(drive._state->RAM[0xFB] | drive._state->RAM[0xFC] << 8);
		tests.check(expected.size() > 2, "reads the file the routine asks for");
		tests.check((drive._state->RAM[0xFD] & FLAG_CARRY) == 0, "kernal LOAD reports success");
		tests.check(end == static_cast<uint16_t>(load_addr + expected.size() - 2),
		            "kernal LOAD returns the end address in X/Y");
		tests.check(std::equal(expected.begin() + 2, expected.end(), drive._state->RAM + load_addr),
		            "kernal LOAD copies the file into RAM");
	}

	// LOAD"$",8 must produce a listable BASIC program, and a missing file must
	// still come back through the kernal's error path.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");

		machine dir;
		dir.insert_disk(d64.data(), d64.size());
		run_until_screen_contains(dir, "READY.", boot_cycle_limit);
		type_line(dir, "LOAD\"$\",8");
		dir.exec(command_cycle_limit);
		type_line(dir, "LIST");
		tests.check(run_until_screen_contains(dir, "1986 ELITE", command_cycle_limit),
		            "LOAD\"$\",8 lists the disk directory");
		tests.check(run_until_screen_contains(dir, "BLOCKS FREE", command_cycle_limit),
		            "directory listing ends with the free block count");

		machine missing;
		missing.insert_disk(d64.data(), d64.size());
		run_until_screen_contains(missing, "READY.", boot_cycle_limit);
		type_line(missing, "LOAD\"NOSUCHFILE\",8");
		tests.check(run_until_screen_contains(missing, "FILE NOT FOUND", command_cycle_limit),
		            "a missing file reports FILE NOT FOUND");
	}

	// A relocating load (no ",1") is the common BASIC case: the kernal must hand
	// back an end address that BASIC stores as its start-of-variables pointer,
	// and a request for a device we do not model must fall through to the ROM.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		std::vector<uint8_t> expected;
		read_disk_file(d64.data(), d64.size(), "1942 D*", expected);

		machine basic_load;
		basic_load.insert_disk(d64.data(), d64.size());
		run_until_screen_contains(basic_load, "READY.", boot_cycle_limit);
		type_line(basic_load, "LOAD\"1942 D*\",8");
		basic_load.exec(command_cycle_limit);

		const auto end = static_cast<uint16_t>(0x0801 + expected.size() - 2);
		const auto vars = static_cast<uint16_t>(basic_load._state->RAM[0x2D] | basic_load._state->RAM[0x2E] << 8);
		tests.check(vars == end, "a relocating load leaves BASIC's start-of-variables at the end address");

		machine other_device;
		other_device.insert_disk(d64.data(), d64.size());
		run_until_screen_contains(other_device, "READY.", boot_cycle_limit);
		type_line(other_device, "LOAD\"1942 D*\",9");
		tests.check(run_until_screen_contains(other_device, "DEVICE NOT PRESENT", command_cycle_limit),
		            "a load from a device we do not model falls through to the ROM");
	}

	// The directory lists DEL entries, as a drive does, with each name's bytes.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		disk_directory dir;
		read_disk_directory(d64.data(), d64.size(), dir);
		const bool lists_del = std::any_of(dir.entries.begin(), dir.entries.end(),
		                                   [](const auto& e) { return e.raw_type == 0x80 && e.type == "DEL"; });
		tests.check(lists_del, "the directory lists DEL entries as a drive does");
		const std::vector<uint8_t> first_name = {'1', '9', '4', '2', ' ', '+', '8', 'D',
		                                         'F', 'H', ' ', ' ', '[', 'N', '0', ']'};
		tests.check(!dir.entries.empty() && dir.entries.front().raw_name == first_name,
		            "a directory entry carries its PETSCII name without the padding");
	}

	// Autostarting a disk types LOAD"name",8,1 and RUN as a user would.
	{
		const auto d64 = read_file(fixture_folder / "1942.d64");
		disk_directory dir;
		read_disk_directory(d64.data(), d64.size(), dir);
		std::vector<uint8_t> expected;
		read_disk_file(d64.data(), d64.size(), "1942 D*", expected);
		const auto& docs = dir.entries[1]; // "1942 DOCS   [N0]"

		machine m;
		const bool started = m.autostart_disk(d64.data(), d64.size(), docs.raw_name.data(), docs.raw_name.size());
		int frames = 0;
		while (m.autostarting() && frames < 1000)
		{
			m.exec(pal_cycles_per_frame);
			++frames;
		}
		const auto load = static_cast<uint16_t>(expected[0] | expected[1] << 8);
		std::cout << "  autostart took " << frames << " frames\n";
		tests.check(started && frames < 400, "an autostart is over within a few seconds of emulated time");
		tests.check(screen_contains(m, "LOAD\"1942 DOCS   "), "an autostart types the LOAD a user would");
		tests.check(std::equal(expected.begin() + 2, expected.begin() + 2 + 64, m._state->RAM + load),
		            "an autostart loads the file it names");
		bool ran = false;
		for (int i = 0; i < 50 && !ran; ++i)
		{
			ran = screen_contains(m, "RUN");
			m.exec(pal_cycles_per_frame);
		}
		tests.check(ran, "an autostart runs a BASIC program with RUN");

		const uint8_t missing[] = {'N', 'O', 'P', 'E'};
		machine untouched;
		tests.check(!untouched.autostart_disk(d64.data(), d64.size(), missing, sizeof(missing)) &&
		                !untouched.has_disk(),
		            "an autostart of a file the disk does not hold leaves the machine alone");
	}

	// An autostart starts the entry it names, though a drive takes the name it
	// types as a pattern: '*' matches the rest of any name, and '?' - typed for
	// what cannot be typed between quotes, such as a comma - any one character.
	{
		const auto first = make_basic_print("THE FIRST FILE");
		const auto named = make_basic_print("THE NAMED FILE");
		const auto starts_named = [&](const std::vector<uint8_t>& d64, const std::string_view name)
		{
			machine m;
			if (!m.autostart_disk(d64.data(), d64.size(), reinterpret_cast<const uint8_t*>(name.data()), name.size()))
				return false;
			for (int frames = 0; m.autostarting() && frames < 1000; ++frames) m.exec(pal_cycles_per_frame);
			return run_until_screen_contains(m, "THE NAMED FILE", command_cycle_limit) &&
			       !screen_contains(m, "THE FIRST FILE");
		};

		const auto stars = make_d64({{"GAME", first}, {"*INTRO*", named}});
		std::vector<uint8_t> read;
		tests.check(read_disk_file_petscii(stars.data(), stars.size(), reinterpret_cast<const uint8_t*>("*INTRO*"), 7,
		                                   read) &&
		                read == first,
		            "as a pattern, a name holding '*' matches a file listed before it");
		tests.check(read_disk_file_entry(stars.data(), stars.size(), reinterpret_cast<const uint8_t*>("*INTRO*"), 7,
		                                 read) &&
		                read == named,
		            "as an entry's name it reads that entry");
		tests.check(starts_named(stars, "*INTRO*"), "an autostart starts the entry whose name holds '*'");

		const auto comma = make_d64({{"GAMEX2", first}, {"GAME,2", named}});
		tests.check(starts_named(comma, "GAME,2"), "and the entry whose name cannot be typed");
	}

	// Autostarting a program puts it in memory once BASIC is ready, then runs it.
	{
		machine m;
		const bool started = m.autostart_prg(prg.data(), prg.size());
		int frames = 0;
		while (m.autostarting() && frames < 1000)
		{
			m.exec(pal_cycles_per_frame);
			++frames;
		}
		tests.check(started && frames < 400, "an autostarted program is placed within a few seconds");
		tests.check(run_until_screen_contains(m, "HELLO WORLD", command_cycle_limit), "an autostarted program runs");

		std::vector<uint8_t> p00(26, 0);
		std::memcpy(p00.data(), "C64File", 7);
		p00.insert(p00.end(), prg.begin(), prg.end());
		machine from_p00;
		tests.check(from_p00.autostart_prg(p00.data(), p00.size()), "an autostart accepts a .p00's program");
	}

	// A .t64 archive's files come out in PRG form.
	{
		std::vector<uint8_t> t64(64 + 32, 0);
		std::memcpy(t64.data(), "C64S tape image file", 20);
		t64[34] = 1; // room for one entry
		t64[36] = 1; // one used
		uint8_t* entry = t64.data() + 64;
		entry[0] = 1;    // a normal tape file
		entry[1] = 0x82; // PRG
		entry[2] = 0x01;
		entry[3] = 0x08; // start $0801
		entry[4] = 0xC6;
		entry[5] = 0xC3; // a bogus end address, as many archives have
		entry[8] = 96;   // data offset
		// NOLINTNEXTLINE(bugprone-not-null-terminated-result): a .t64 name is 16 bytes padded with spaces
		std::memcpy(entry + 16, "EXAMPLE         ", 16);
		t64.insert(t64.end(), prg.begin() + 2, prg.end());

		std::vector<tape_entry> files;
		std::vector<uint8_t> extracted;
		tests.check(read_tape_directory(t64.data(), t64.size(), files) && files.size() == 1 &&
		                files[0].filename == "EXAMPLE" && files[0].length == prg.size() - 2,
		            "a .t64 lists its files, cut to the data it holds");
		tests.check(read_tape_file(t64.data(), t64.size(), "example", extracted) && extracted == prg,
		            "a .t64 file comes out in PRG form");

		const uint8_t stored[] = {'E', 'X', 'A', 'M', 'P', 'L', 'E'};
		const uint8_t folded[] = {'e', 'x', 'a', 'm', 'p', 'l', 'e'};
		tests.check(read_tape_file_petscii(t64.data(), t64.size(), stored, sizeof(stored), extracted) &&
		                extracted == prg,
		            "a .t64 file is found by the name it stores");
		tests.check(!read_tape_file_petscii(t64.data(), t64.size(), folded, sizeof(folded), extracted),
		            "a stored .t64 name is matched byte for byte, with nothing folded");

		const uint8_t pattern[] = {'E', 'X', '*'};
		tests.check(read_tape_file_entry(t64.data(), t64.size(), stored, sizeof(stored), extracted) &&
		                extracted == prg &&
		                !read_tape_file_entry(t64.data(), t64.size(), pattern, sizeof(pattern), extracted),
		            "a .t64 entry is read by the name it stores, in which '*' is not a pattern");
	}

	std::cout << "lib64: " << tests.checks << " checks, " << tests.failures << " failed\n";
	return tests.failures == 0 ? 0 : 1;
}
