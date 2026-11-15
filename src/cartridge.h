#pragma once

// lib64 by Zac Walker
//
// C64 cartridge (.CRT) model: hardware types and bank-switching state.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// C64 cartridge hardware types (subset with bank-switching support), matching
// the .CRT format's 16-bit hardware type field.
namespace cart_type
{
	constexpr int NORMAL = 0;
	constexpr int ACTION_REPLAY = 1;
	constexpr int FINAL_CARTRIDGE_III = 3;
	constexpr int SIMONS_BASIC = 4;
	constexpr int OCEAN_TYPE_1 = 5;
	constexpr int FUN_PLAY = 7;
	constexpr int SUPER_GAMES = 8;
	constexpr int ATOMIC_POWER = 9;
	constexpr int EPYX_FASTLOAD = 10;
	constexpr int WESTERMANN = 11;
	constexpr int FINAL_CARTRIDGE_I = 13;
	constexpr int C64_GAME_SYSTEM = 15;
	constexpr int WARPSPEED = 16;
	constexpr int DINAMIC = 17;
	constexpr int ZAXXON = 18;
	constexpr int MAGIC_DESK = 19;
	constexpr int COMAL_80 = 21;
	constexpr int ROSS = 23;
	constexpr int EASYFLASH = 32;
	constexpr int RGCD = 57;
}

// A single ROM/RAM chip bank parsed from a CHIP packet in a .CRT file.
struct chip_bank
{
	uint16_t bank_number = 0;
	uint16_t load_address = 0x8000;
	int size = 0;
	std::vector<uint8_t> data;
};

// C64 cartridge: parses a .CRT image, exposes the currently banked-in ROML/ROMH
// contents and implements per-type bank switching through the $DE00-$DFFF I/O
// window.
class cartridge
{
public:
	bool enabled = false;
	int exrom = 1; // /EXROM line, active low (0 = asserted)
	int game = 1;  // /GAME line, active low (0 = asserted)

	// Parse a .CRT image. Returns false on a malformed file, leaving the
	// cartridge as it was.
	bool load(const uint8_t* bytes, size_t length);
	void reset();
	void eject();

	// ROML ($8000-$9FFF) / ROMH ($A000-$BFFF or $E000-$FFFF) fetch. Returns true
	// and sets out when the cartridge serves the address. A chip smaller than
	// its window repeats through it.
	bool read(uint16_t addr, uint8_t& out) const;
	// A CPU read of ROML, which on some hardware also switches banks.
	void touch_roml(uint16_t addr);

	// What the VIC-II fetches at $3000-$3FFF of its bank in Ultimax mode: the
	// top 4K of ROMH. False outside Ultimax mode, where the VIC sees RAM.
	bool vic_read(uint16_t vic_address, uint8_t& out) const;

	// I/O window ($DE00-$DFFF) accesses. Some cartridges bank-switch on read.
	bool read_io(uint16_t addr, uint8_t& out);
	void write_io(uint16_t addr, uint8_t value);

	bool has_roml() const { return roml_index >= 0; }
	bool has_romh() const { return romh_index >= 0; }
	int hardware_type() const { return hw_type; }
	const std::string& name() const { return cart_name; }

private:
	std::string cart_name;
	int hw_type = cart_type::NORMAL;
	int header_exrom = 1;
	int header_game = 1;
	std::vector<chip_bank> banks;
	int current_bank = 0;
	int roml_index = -1;
	int romh_index = -1;
	std::vector<uint8_t> easyflash_ram;
	uint8_t easyflash_control = 0;

	bool parse(const uint8_t* bytes, size_t length);
	void update_bank_mapping();
	void get_mem_config(int& roml_addr, int& romh_addr) const;
	// A byte of the ROMH chip mapped now, at an offset into its 8K window.
	bool romh_byte(int offset, uint8_t& out) const;
};
