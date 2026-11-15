// lib64 by Zac Walker
//
// VIC-II raster renderer. Renders one raster line at a time into a 384x272
// packed-RGB frame buffer, supporting every C64 graphics mode plus the eight
// hardware sprites. Reads the live VIC state from its register file and its
// memory as the chip sees it, so raster-split effects work when the CPU
// changes registers mid-frame.

#include "machine.h"
#include "vic.h"

#include <cstring>

// clang-format off
const uint32_t vic_palette[16] = {
	0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
	0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
	0x6F4F25, 0x433900, 0x9A6759, 0x444444,
	0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};
// clang-format on

namespace
{
	constexpr int SCREEN_W = 320;
	constexpr int SCREEN_H = 200;
	constexpr int BORDER_X = (VIC_FB_WIDTH - SCREEN_W) / 2;  // 32
	constexpr int BORDER_Y = (VIC_FB_HEIGHT - SCREEN_H) / 2; // 36

	// The PAL X counter runs to $1F7; a sprite placed past the right edge, from
	// $1F8 back by its widest, shows at the left instead, and $1F8 and above
	// never match at all.
	constexpr int SPRITE_X_WRAP = 0x1F8;

	// The 16K the VIC-II sees, as the chip sees it: RAM, but for the character
	// ROM at $1000-$1FFF of banks 0 and 2 and, in Ultimax mode, the cartridge's
	// ROMH at $3000-$3FFF of every bank in its place.
	struct vic_memory
	{
		const machine_state* s;
		int bank;
		bool ultimax;

		uint8_t operator()(const int address) const
		{
			const int a = address & 0x3FFF;
			if (ultimax)
			{
				uint8_t out = 0;
				if (s->cart.vic_read(static_cast<uint16_t>(a), out)) return out;
			}
			else if ((bank & 1) == 0 && (a & 0x3000) == 0x1000)
			{
				return rom_chars[a & 0x0FFF];
			}
			return s->RAM[(bank << 14) | a];
		}
	};

	struct line_context
	{
		vic_memory mem;
		const uint8_t* regs;
		const uint8_t* colour;
		uint32_t* line;  // this raster line of the frame buffer
		uint8_t* fgmask; // 1 where the graphics are foreground, for sprite collisions
		int screen;      // video matrix, within the bank
		int chars;       // character generator, within the bank
		int bitmap;      // bitmap, within the bank
		int row;         // character row, 0-24
		int pixel_y;     // line within the row, 0-7
		int scroll_x;
	};

	line_context make_context(const machine_state* s, uint32_t* line, uint8_t* fgmask)
	{
		const uint8_t* regs = s->vic.regs;
		// VIC bank from CIA2 port A bits 0-1, inverted. Lines set to input float
		// high, which is why a machine that has not programmed the DDR yet (a bare
		// reset) sees bank 0.
		const int port_a = (s->cia2.regs[0] & s->cia2.regs[2]) | (0xFF & ~s->cia2.regs[2]);
		const bool ultimax = s->cart.enabled && s->cart.exrom == 1 && s->cart.game == 0;

		line_context c{};
		c.mem = {s, (~port_a) & 0x03, ultimax};
		c.regs = regs;
		c.colour = s->color_ram;
		c.line = line;
		c.fgmask = fgmask;
		c.screen = ((regs[0x18] >> 4) & 0x0F) * 0x0400;
		c.chars = ((regs[0x18] >> 1) & 0x07) * 0x0800;
		c.bitmap = (regs[0x18] & 0x08) ? 0x2000 : 0x0000;
		c.scroll_x = regs[0x16] & 0x07;
		return c;
	}

	void plot(const line_context& c, const int x, const uint32_t colour, const bool foreground)
	{
		if (x < 0 || x >= VIC_FB_WIDTH) return;
		c.line[x] = colour;
		if (foreground) c.fgmask[x] = 1;
	}

	void render_standard_character(const line_context& c, const int code_mask)
	{
		for (int col = 0; col < 40; col++)
		{
			const int cell = c.row * 40 + col;
			const int code = c.mem(c.screen + cell) & code_mask;
			const uint32_t colour = vic_palette[c.colour[cell] & 0x0F];
			const uint8_t bits = c.mem(c.chars + code * 8 + c.pixel_y);
			const int x0 = BORDER_X + col * 8 + c.scroll_x;
			for (int px = 0; px < 8; px++)
				if (bits & (0x80 >> px)) plot(c, x0 + px, colour, true);
		}
	}

	void render_multicolor_character(const line_context& c, const int code_mask)
	{
		const uint32_t bg[3] = {
			vic_palette[c.regs[0x21] & 0x0F],
			vic_palette[c.regs[0x22] & 0x0F],
			vic_palette[c.regs[0x23] & 0x0F],
		};

		for (int col = 0; col < 40; col++)
		{
			const int cell = c.row * 40 + col;
			const int code = c.mem(c.screen + cell) & code_mask;
			const uint8_t colour_ram = c.colour[cell];
			const uint8_t bits = c.mem(c.chars + code * 8 + c.pixel_y);
			const int x0 = BORDER_X + col * 8 + c.scroll_x;

			if ((colour_ram & 0x08) == 0)
			{
				// Colour RAM bit 3 clear: this cell is a hires character.
				const uint32_t colour = vic_palette[colour_ram & 0x07];
				for (int px = 0; px < 8; px++)
					if (bits & (0x80 >> px)) plot(c, x0 + px, colour, true);
				continue;
			}

			for (int pair = 0; pair < 4; pair++)
			{
				const int value = (bits >> (6 - pair * 2)) & 0x03;
				const uint32_t colour = value == 3 ? vic_palette[colour_ram & 0x07] : bg[value];
				// Pairs 10 and 11 are foreground for collisions; 00 and 01 are not.
				plot(c, x0 + pair * 2, colour, value >= 2);
				plot(c, x0 + pair * 2 + 1, colour, value >= 2);
			}
		}
	}

	void render_standard_bitmap(const line_context& c)
	{
		for (int col = 0; col < 40; col++)
		{
			const int cell = c.row * 40 + col;
			const uint8_t colours = c.mem(c.screen + cell);
			const uint32_t fg = vic_palette[(colours >> 4) & 0x0F];
			const uint32_t bg = vic_palette[colours & 0x0F];
			const uint8_t bits = c.mem(c.bitmap + cell * 8 + c.pixel_y);
			const int x0 = BORDER_X + col * 8 + c.scroll_x;
			for (int px = 0; px < 8; px++)
			{
				const bool set = (bits & (0x80 >> px)) != 0;
				plot(c, x0 + px, set ? fg : bg, set);
			}
		}
	}

	void render_multicolor_bitmap(const line_context& c)
	{
		const uint32_t bg0 = vic_palette[c.regs[0x21] & 0x0F];

		for (int col = 0; col < 40; col++)
		{
			const int cell = c.row * 40 + col;
			const uint8_t colours = c.mem(c.screen + cell);
			const uint32_t palette[4] = {
				bg0,
				vic_palette[(colours >> 4) & 0x0F],
				vic_palette[colours & 0x0F],
				vic_palette[c.colour[cell] & 0x0F],
			};
			const uint8_t bits = c.mem(c.bitmap + cell * 8 + c.pixel_y);
			const int x0 = BORDER_X + col * 8 + c.scroll_x;
			for (int pair = 0; pair < 4; pair++)
			{
				const int value = (bits >> (6 - pair * 2)) & 0x03;
				plot(c, x0 + pair * 2, palette[value], value >= 2);
				plot(c, x0 + pair * 2 + 1, palette[value], value >= 2);
			}
		}
	}

	void render_extended_background(const line_context& c)
	{
		const uint32_t bg[4] = {
			vic_palette[c.regs[0x21] & 0x0F],
			vic_palette[c.regs[0x22] & 0x0F],
			vic_palette[c.regs[0x23] & 0x0F],
			vic_palette[c.regs[0x24] & 0x0F],
		};

		for (int col = 0; col < 40; col++)
		{
			const int cell = c.row * 40 + col;
			const int code = c.mem(c.screen + cell);
			const uint32_t fg = vic_palette[c.colour[cell] & 0x0F];
			const uint8_t bits = c.mem(c.chars + (code & 0x3F) * 8 + c.pixel_y);
			const int x0 = BORDER_X + col * 8 + c.scroll_x;
			for (int px = 0; px < 8; px++)
			{
				const bool set = (bits & (0x80 >> px)) != 0;
				plot(c, x0 + px, set ? fg : bg[(code >> 6) & 0x03], set);
			}
		}
	}

	void render_sprites(machine_state* s, const line_context& c, const int raster_line)
	{
		uint8_t* const regs = s->vic.regs;
		const uint8_t sprite_enable = regs[0x15];
		if (sprite_enable == 0) return;

		int8_t* collision = s->sprite_collision;
		std::memset(collision, 0xFF, VIC_FB_WIDTH);

		const uint8_t x_expand = regs[0x1D];
		const uint8_t y_expand = regs[0x17];
		const uint8_t multicolor = regs[0x1C];
		const uint8_t priority = regs[0x1B];
		const uint32_t mc0 = vic_palette[regs[0x25] & 0x0F];
		const uint32_t mc1 = vic_palette[regs[0x26] & 0x0F];

		// A collision latches even when the sprite is drawn behind the graphics, so
		// compare the registers either side of the whole scanline and raise the
		// interrupt once rather than per pixel.
		const uint8_t sprite_sprite_before = regs[0x1E];
		const uint8_t sprite_bg_before = regs[0x1F];

		// Render 7..0 so lower-numbered sprites end up on top.
		for (int sprite = 7; sprite >= 0; sprite--)
		{
			const uint8_t bit = static_cast<uint8_t>(1 << sprite);
			if (!(sprite_enable & bit)) continue;

			// A sprite at Y is drawn from raster Y + 1.
			const bool y_exp = (y_expand & bit) != 0;
			const int row_in_sprite = raster_line - (regs[0x01 + sprite * 2] + 1);
			if (row_in_sprite < 0 || row_in_sprite >= (y_exp ? 42 : 21)) continue;

			int vic_x = regs[0x00 + sprite * 2] | ((regs[0x10] & bit) ? 0x100 : 0);
			if (vic_x >= SPRITE_X_WRAP) continue;
			if (vic_x >= SPRITE_X_WRAP - 48) vic_x -= SPRITE_X_WRAP;
			const int sprite_x = vic_x - 24 + BORDER_X;

			const int row = y_exp ? row_in_sprite / 2 : row_in_sprite;
			const int data = c.mem(c.screen + 0x03F8 + sprite) * 64 + row * 3;
			const uint32_t colour = vic_palette[regs[0x27 + sprite] & 0x0F];
			const bool is_mc = (multicolor & bit) != 0;
			const bool behind = (priority & bit) != 0;
			const int x_step = (x_expand & bit) ? 2 : 1;

			const auto paint = [&](const int px, const uint32_t pixel)
			{
				if (px < 0 || px >= VIC_FB_WIDTH) return;
				// NOLINTNEXTLINE(bugprone-signed-char-misuse): a sprite number, or -1 where no sprite has drawn
				const int other = collision[px];
				if (other != -1) regs[0x1E] |= static_cast<uint8_t>(bit | (1 << other));
				collision[px] = static_cast<int8_t>(sprite);
				if (c.fgmask[px]) regs[0x1F] |= bit;
				if (behind && c.fgmask[px]) return;
				c.line[px] = pixel;
			};

			for (int byte_col = 0; byte_col < 3; byte_col++)
			{
				const uint8_t bits = c.mem(data + byte_col);
				if (is_mc)
				{
					for (int pair = 0; pair < 4; pair++)
					{
						const int value = (bits >> (6 - pair * 2)) & 0x03;
						if (value == 0) continue;
						const uint32_t pixel = value == 1 ? mc0 : value == 2 ? colour : mc1;
						const int base_x = sprite_x + (byte_col * 8 + pair * 2) * x_step;
						for (int dx = 0; dx < 2 * x_step; dx++) paint(base_x + dx, pixel);
					}
				}
				else
				{
					for (int px = 0; px < 8; px++)
					{
						if (!(bits & (0x80 >> px))) continue;
						const int base_x = sprite_x + (byte_col * 8 + px) * x_step;
						for (int dx = 0; dx < x_step; dx++) paint(base_x + dx, colour);
					}
				}
			}
		}

		uint8_t latched = 0;
		if (regs[0x1E] != sprite_sprite_before) latched |= 0x02; // $D019 sprite-sprite
		if (regs[0x1F] != sprite_bg_before) latched |= 0x04;     // $D019 sprite-background
		if (latched)
		{
			s->vic.irq_status |= latched;
			s->update_vic_irq();
		}
	}
}

void vic_render_scanline(machine_state* s, const int raster_line)
{
	const int canvas_y = raster_line - VIC_FIRST_VISIBLE_RASTER;
	if (canvas_y < 0 || canvas_y >= VIC_FB_HEIGHT) return;

	const uint8_t* regs = s->vic.regs;
	uint32_t* line = s->framebuffer + canvas_y * VIC_FB_WIDTH;
	const uint32_t border = vic_palette[regs[0x20] & 0x0F];

	// The vertical window is rows 51-250, or 55-246 with RSEL clear, and DEN
	// clear keeps the border down all frame. The border covers the sprites too.
	const int screen_y = canvas_y - BORDER_Y;
	const bool rsel = (regs[VIC_CTRL1 & 0x3F] & 0x08) != 0;
	const bool display_enabled = (regs[VIC_CTRL1 & 0x3F] & 0x10) != 0;
	const bool in_window = display_enabled &&
	                       (rsel ? screen_y >= 0 && screen_y < SCREEN_H : screen_y >= 4 && screen_y < SCREEN_H - 4);
	if (!in_window)
	{
		for (int x = 0; x < VIC_FB_WIDTH; x++) line[x] = border;
		return;
	}

	const uint32_t background = vic_palette[regs[0x21] & 0x0F];
	for (int x = 0; x < VIC_FB_WIDTH; x++) line[x] = background;
	std::memset(s->fg_mask, 0, VIC_FB_WIDTH);

	// YSCROLL 3 is the neutral position: the first badline is raster 0x30+YSCROLL,
	// so the KERNAL default of 3 starts screen row 0 at the top of the window.
	const int yscroll = regs[VIC_CTRL1 & 0x3F] & 0x07;
	const int adjusted_y = screen_y - (yscroll - 3);
	line_context c = make_context(s, line, s->fg_mask);

	if (adjusted_y >= 0 && adjusted_y < SCREEN_H)
	{
		c.row = adjusted_y >> 3;
		c.pixel_y = adjusted_y & 7;

		const bool ecm = (regs[VIC_CTRL1 & 0x3F] & 0x40) != 0;
		const bool bmm = (regs[VIC_CTRL1 & 0x3F] & 0x20) != 0;
		const bool mcm = (regs[VIC_CTRL2 & 0x3F] & 0x10) != 0;

		if (bmm && mcm) render_multicolor_bitmap(c);
		else if (bmm) render_standard_bitmap(c);
		else if (ecm && !mcm) render_extended_background(c);
		else if (mcm) render_multicolor_character(c, ecm ? 0x3F : 0xFF);
		else render_standard_character(c, 0xFF);

		// ECM with BMM or MCM is invalid: the chip still fetches, so sprites
		// still collide with the graphics, but it draws them black.
		if (ecm && (bmm || mcm))
			for (int x = BORDER_X; x < BORDER_X + SCREEN_W; x++) line[x] = vic_palette[0];
	}

	render_sprites(s, c, raster_line);

	// The side borders, over everything. CSEL clear narrows the window to 38
	// columns, seven pixels in on the left and nine on the right.
	const bool csel = (regs[VIC_CTRL2 & 0x3F] & 0x08) != 0;
	const int left = csel ? BORDER_X : BORDER_X + 7;
	const int right = csel ? BORDER_X + SCREEN_W : BORDER_X + SCREEN_W - 9;
	for (int x = 0; x < left; x++) line[x] = border;
	for (int x = right; x < VIC_FB_WIDTH; x++) line[x] = border;
}
