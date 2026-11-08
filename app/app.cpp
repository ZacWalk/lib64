// lib8bit by Zac Walker
//
// The C64 host is an application of libdesktop's platform layer. The emulator
// remains a byte-in/byte-out library: this file owns media files, the C64
// keyboard layout, presentation, menus, settings and the audio clock.

#include "debug.h"
#include "disk.h"
#include "machine.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/menu_bar.h"
#include "libdesktop/ui/test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	constexpr std::string_view kProductName = "lib8bit";
	constexpr int kFramebufferW = machine::framebuffer_width();
	constexpr int kFramebufferH = machine::framebuffer_height();
	constexpr int kSampleRate = 44100;
	constexpr int kCyclesPerFrame = pal_cycles_per_frame;
	constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
	constexpr int kAudioBlocks = 4;

	constexpr int kCmdOpen = 1001;
	constexpr int kCmdReset = 1002;
	constexpr int kCmdExit = 1003;
	constexpr int kCmdAbout = 1004;
	constexpr int kCmdPause = 1005;
	constexpr int kCmdEject = 1006;
	constexpr int kCmdKeyboard = 1007;
	constexpr int kCmdDebug = 1008;
	constexpr int kCmdArrowsCursor = 1010;
	constexpr int kCmdArrowsJoy1 = 1011;
	constexpr int kCmdArrowsJoy2 = 1012;

	constexpr uint8_t kJoyUp = 0x01;
	constexpr uint8_t kJoyDown = 0x02;
	constexpr uint8_t kJoyLeft = 0x04;
	constexpr uint8_t kJoyRight = 0x08;
	constexpr uint8_t kJoyFire = 0x10;


	enum class arrow_mode
	{
		cursor,
		joystick1,
		joystick2,
	};

	struct keypos
	{
		int row = 0;
		int col = 0;
	};

	machine g_machine;
	class screen_element;

	struct app_surface
	{
		int width = 0;
		int height = 0;
		std::vector<uint32_t> pixels;
	};

	ui::frame_ptr g_frame;
	std::shared_ptr<screen_element> g_screen;
	std::mutex g_machine_mutex;
	app_surface g_surface;
	core::surface_ptr g_texture_surface;

	struct frame_snapshot
	{
		std::vector<uint32_t> pixels = std::vector<uint32_t>(
			static_cast<size_t>(kFramebufferW) * kFramebufferH);
		debug_state debug;
	};

	std::mutex g_frame_mutex;
	frame_snapshot g_frames[3];
	int g_frame_write = 0;
	int g_frame_ready = 1;
	int g_frame_read = 2;
	bool g_frame_fresh = false;

	std::atomic<bool> g_running{true};
	std::atomic<bool> g_worker_stop{false};
	std::atomic<bool> g_worker_active{false};
	std::atomic<bool> g_shutting_down{false};
	std::atomic<bool> g_ui_tick_pending{false};
	std::atomic<bool> g_sid_stopped_pending{false};
	std::atomic<uint32_t> g_media_generation{0};
	std::atomic<uint32_t> g_sid_stopped_generation{0};
	std::atomic<bool> g_audio_failure_pending{false};
	std::atomic<bool> g_restore_sent{false};
	bool g_headless = false;
	std::atomic<bool> g_paused{false};
	std::atomic<bool> g_show_debug{false};
	std::atomic<bool> g_audio_ok{false};
	bool g_sound_started = false;
	std::atomic<bool> g_sid_playing{false};
	std::atomic<arrow_mode> g_arrow_mode{arrow_mode::cursor};
	std::atomic<uint8_t> g_joystick{0};
	std::atomic<uint8_t> g_joystick_taps{0};
	std::atomic<int64_t> g_emu_frames{0};

	std::string g_media;
	bool g_cart_inserted = false;
	bool g_disk_inserted = false;
	core::file_path g_start_path;
	platform::serial_executor_ptr g_emu_executor;

	struct ui_settings
	{
		uint32_t panel_w96 = 0;
		bool show_debug = false;
		core::recti placement;
		bool has_placement = false;
		bool maximized = false;
	};

	platform::setting_file_ptr g_settings_store;

	ui_settings g_settings;
	bool g_settings_available = false;
	int g_dpi = 96;
	int g_panel_w = 0;
	bool g_dragging = false;
	core::sizei g_minimum_client_size;
	int g_menu_height = 0;
	bool g_end_session_saved = false;

	struct picture_layout
	{
		int x = 0;
		int y = 0;
		int scale = 1;
		int width = kFramebufferW;
		int height = kFramebufferH;
		int picture_width = 0;
		int splitter = -1;
		bool debug = false;

		bool operator==(const picture_layout&) const = default;
	};

	core::sizei g_client;
	picture_layout g_layout;
	std::vector<int16_t> g_audio_scratch(4096);

	struct held_key
	{
		char32_t key = 0;
		std::array<keypos, 2> positions{};
		int count = 0;
	};

	std::vector<held_key> g_held_keys;
	std::vector<ui::command_ptr> g_menus;

	int config_int(const std::string_view section, const std::string_view key, const int fallback)
	{
		if (!g_settings_store) return fallback;
		std::string text;
		if (!g_settings_store->read(section, key, text)) return fallback;
		int value = fallback;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		return parsed.ec == std::errc{} ? value : fallback;
	}

	void configure_settings()
	{
		if (g_headless) return;
		const auto roaming = platform::known_path(platform::known_folder::roaming_app_data);
		if (roaming.is_empty()) return;

		g_settings_store = platform::create_ini_file_settings(roaming.combine("lib8bit"));
		g_settings_available = g_settings_store != nullptr;
	}

	void load_ui_settings()
	{
		if (!g_settings_available) return;
		g_settings.panel_w96 = static_cast<uint32_t>(std::max(0, config_int("panel", "width", 0)));
		g_settings.show_debug = config_int("panel", "visible", 0) != 0;

		const auto x = config_int("window", "x", std::numeric_limits<int>::min());
		const auto y = config_int("window", "y", std::numeric_limits<int>::min());
		const auto width = config_int("window", "width", 0);
		const auto height = config_int("window", "height", 0);
		if (x != std::numeric_limits<int>::min() && y != std::numeric_limits<int>::min() &&
			width > 0 && height > 0)
		{
			g_settings.placement = {x, y, x + width, y + height};
			g_settings.has_placement = true;
		}
		g_settings.maximized = config_int("window", "maximized", 0) != 0;
	}

	void save_settings()
	{
		if (!g_frame) return;
		g_end_session_saved = true;
		if (g_headless || !g_settings_available || !g_settings_store) return;

		auto bounds = g_frame->window_bounds();
		if (g_frame->is_maximized() && g_settings.has_placement) bounds = g_settings.placement;
		if (bounds.width() > 0 && bounds.height() > 0)
		{
			g_settings_store->write("window", "x", std::to_string(bounds.left));
			g_settings_store->write("window", "y", std::to_string(bounds.top));
			g_settings_store->write("window", "width", std::to_string(bounds.width()));
			g_settings_store->write("window", "height", std::to_string(bounds.height()));
		}
		g_settings_store->write("window", "maximized", std::to_string(g_frame->is_maximized() ? 1 : 0));
		g_settings_store->write("panel", "width", std::to_string(std::max(0,
			core::mul_div(g_panel_w, 96, std::max(1, g_dpi)))));
		g_settings_store->write("panel", "visible", std::to_string(g_show_debug.load() ? 1 : 0));
	}

	void report(const std::string_view message)
	{
		if (g_frame)
			platform::message_box(nullptr, message, kProductName,
				{platform::message_box_buttons::ok, platform::message_box_icon::warning});
		else
			platform::show_error(message);
	}

	std::vector<uint8_t> read_file(const core::file_path& path)
	{
		const auto file = platform::open_file(path, platform::file_open_mode::read);
		if (!file) return {};

		const auto size64 = file->size();
		if (size64 == 0 || size64 > kMaxFileSize) return {};

		std::vector<uint8_t> bytes(static_cast<size_t>(size64));
		size_t offset = 0;
		while (offset < bytes.size())
		{
			const auto read = file->read(bytes.data() + offset, bytes.size() - offset);
			if (read == 0) return {};
			offset += static_cast<size_t>(read);
		}
		return bytes;
	}

	bool has_signature(const std::vector<uint8_t>& bytes, const std::string_view signature)
	{
		return bytes.size() >= signature.size() &&
			std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
	}

	bool is_crt(const std::vector<uint8_t>& bytes)
	{
		return has_signature(bytes, "C64 CARTRIDGE   ");
	}

	bool is_sid(const std::vector<uint8_t>& bytes)
	{
		return has_signature(bytes, "PSID") || has_signature(bytes, "RSID");
	}

	bool named_like_prg(const core::file_path& path)
	{
		const auto extension = path.extension();
		return extension.empty() || core::icmp(extension, ".prg") == 0;
	}

	void stop_audio();
	void stop_emulation_worker();
	void reset_audio_clock();
	void update_title();

	void load_file(const core::file_path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			report("Failed to read file.");
			return;
		}

		std::string error;
		bool loaded = false;
		bool cart = g_cart_inserted;
		bool disk = g_disk_inserted;

		{
			std::lock_guard lock(g_machine_mutex);
			if (is_sid(bytes))
			{
				loaded = g_machine.load_sid(bytes.data(), bytes.size());
				if (!loaded) error = "Not a valid SID file.";
			}
			else if (is_crt(bytes))
			{
				loaded = g_machine.load_crt(bytes.data(), bytes.size());
				cart = loaded;
				if (!loaded) error = "Not a valid CRT file.";
			}
			else if (g_machine.insert_disk(bytes.data(), bytes.size()))
			{
				// Keep the image mounted after the first program starts. The KERNAL
				// LOAD trap can then service later loads from the same bytes.
				disk = true;
				loaded = true;
				g_machine.reset();
				g_machine.exec(3'000'000);

				std::vector<uint8_t> prg;
				if (!read_disk_file(bytes.data(), bytes.size(), "*", prg))
					error = "No program found on this disk. It is still in the drive.";
				else if (!g_machine.load_prg(prg.data(), prg.size()))
					error = "The program on this disk could not be started.";
			}
			else if (!named_like_prg(path))
			{
				error = "Not a C64 program, cartridge, SID tune or disk image.";
			}
			else if (!machine::is_prg(bytes.data(), bytes.size()))
			{
				error = "Not a valid PRG file.";
			}
			else
			{
				// Boot first so a BASIC program is injected exactly as the old host
				// did, rather than relying on an implementation detail of KERNAL RAM.
				g_machine.reset();
				g_machine.exec(3'000'000);
				loaded = g_machine.load_prg(bytes.data(), bytes.size());
				if (!loaded) error = "Not a valid PRG file.";
			}

			if (loaded)
			{
				reset_audio_clock();
				g_machine.set_audio_enabled(true);
				g_sid_playing.store(g_machine.sid_player_active());
			}
		}

		if (loaded)
		{
			g_media_generation.fetch_add(1);
			g_cart_inserted = cart;
			g_disk_inserted = disk;
			g_media = path.name();
			update_title();
		}

		if (!error.empty()) report(error);
	}

	void open_dialog()
	{
		const std::vector<platform::file_dialog_filter> filters = {
			{"C64 Files", "*.prg;*.crt;*.sid;*.d64;*.d71;*.d81"},
			{"Programs", "*.prg"},
			{"Cartridges", "*.crt"},
			{"SID Tunes", "*.sid;*.psid"},
			{"Disk Images", "*.d64;*.d71;*.d81"},
			{"All Files", "*.*"},
		};
		std::vector<core::file_path> paths;
		if (platform::prompt_for_open_paths(nullptr, paths, filters, false) && !paths.empty())
			load_file(paths.front());
	}

	void release_all_keys()
	{
		g_held_keys.clear();
		std::lock_guard lock(g_machine_mutex);
		g_joystick.store(0);
		g_joystick_taps.store(0);
		g_restore_sent.store(false);
		g_machine.clear_keys();
	}

	void set_arrow_mode(const arrow_mode mode)
	{
		g_arrow_mode.store(mode);
		if (mode == arrow_mode::cursor)
		{
			g_joystick.store(0);
			g_joystick_taps.store(0);
		}
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void apply_joystick()
	{
		const arrow_mode mode = g_arrow_mode.load();
		if (mode == arrow_mode::cursor)
		{
			g_machine.set_joystick(1, false, false, false, false, false);
			g_machine.set_joystick(2, false, false, false, false, false);
			return;
		}

		const uint8_t bits = static_cast<uint8_t>(g_joystick.load() | g_joystick_taps.exchange(0));
		const int port = mode == arrow_mode::joystick1 ? 1 : 2;
		const int other = port == 1 ? 2 : 1;
		g_machine.set_joystick(port, (bits & kJoyUp) != 0, (bits & kJoyDown) != 0,
			(bits & kJoyLeft) != 0, (bits & kJoyRight) != 0, (bits & kJoyFire) != 0);
		g_machine.set_joystick(other, false, false, false, false, false);
	}

	void send_keys(const keypos* positions, const int count, const bool pressed)
	{
		if (count <= 0) return;
		std::lock_guard lock(g_machine_mutex);

		if (g_paused.load())
		{
			g_machine.flush_keys();
			for (int i = 0; i < count; ++i)
				g_machine.set_key(positions[i].row, positions[i].col, pressed);
		}
		else
		{
			for (int i = 0; i < count; ++i)
				g_machine.queue_key(positions[i].row, positions[i].col, pressed);
		}
	}

	// The map deliberately follows VICE's positional layout. Shifted cursor
	// keys and function keys press the C64's left shift as a second matrix key.
	int map_key(const unsigned int vk, keypos positions[2])
	{
		const auto one = [&](const int row, const int col)
		{
			positions[0] = {row, col};
			return 1;
		};
		const auto shifted = [&](const int row, const int col)
		{
			positions[0] = {1, 7};
			positions[1] = {row, col};
			return 2;
		};

		const auto key = static_cast<char32_t>(vk);
		switch (key)
		{
		case U'A': return one(1, 2); case U'B': return one(3, 4); case U'C': return one(2, 4);
		case U'D': return one(2, 2); case U'E': return one(1, 6); case U'F': return one(2, 5);
		case U'G': return one(3, 2); case U'H': return one(3, 5); case U'I': return one(4, 1);
		case U'J': return one(4, 2); case U'K': return one(4, 5); case U'L': return one(5, 2);
		case U'M': return one(4, 4); case U'N': return one(4, 7); case U'O': return one(4, 6);
		case U'P': return one(5, 1); case U'Q': return one(7, 6); case U'R': return one(2, 1);
		case U'S': return one(1, 5); case U'T': return one(2, 6); case U'U': return one(3, 6);
		case U'V': return one(3, 7); case U'W': return one(1, 1); case U'X': return one(2, 7);
		case U'Y': return one(3, 1); case U'Z': return one(1, 4);
		case U'1': return one(7, 0); case U'2': return one(7, 3); case U'3': return one(1, 0);
		case U'4': return one(1, 3); case U'5': return one(2, 0); case U'6': return one(2, 3);
		case U'7': return one(3, 0); case U'8': return one(3, 3); case U'9': return one(4, 0);
		case U'0': return one(4, 3);
		default: break;
		}

		if (key == platform::keys::OEM_COMMA) return one(5, 7);
		if (key == platform::keys::OEM_PERIOD) return one(5, 4);
		if (key == platform::keys::OEM_2) return one(6, 7);
		if (key == platform::keys::OEM_1) return one(6, 2);
		if (key == platform::keys::OEM_MINUS) return one(5, 3);
		if (key == platform::keys::OEM_PLUS) return one(5, 0);
		if (key == platform::keys::OEM_3) return one(7, 1);
		if (key == platform::keys::OEM_7) return one(5, 5);
		if (key == platform::keys::OEM_4) return one(5, 6);
		if (key == platform::keys::OEM_6) return one(6, 1);
		if (key == platform::keys::OEM_5 || key == platform::keys::OEM_102) return one(6, 6);
		if (key == platform::keys::NEXT) return one(6, 5);
		if (key == platform::keys::END) return one(6, 0);
		if (key == platform::keys::RETURN) return one(0, 1);
		if (key == platform::keys::SPACE) return one(7, 4);
		if (key == platform::keys::BACK || key == platform::keys::DEL) return one(0, 0);
		if (key == platform::keys::SHIFT) return one(1, 7);
		if (key == platform::keys::CONTROL) return one(7, 2);
		if (key == platform::keys::TAB) return one(7, 5);
		if (key == platform::keys::ESCAPE) return one(7, 7);
		if (key == platform::keys::HOME) return one(6, 3);
		if (key == platform::keys::DOWN) return one(0, 7);
		if (key == platform::keys::RIGHT) return one(0, 2);
		if (key == platform::keys::UP) return shifted(0, 7);
		if (key == platform::keys::LEFT) return shifted(0, 2);
		if (key == platform::keys::F1) return one(0, 4);
		if (key == platform::keys::F3) return one(0, 5);
		if (key == platform::keys::F5) return one(0, 6);
		if (key == platform::keys::F7) return one(0, 3);
		if (key == platform::keys::F2) return shifted(0, 4);
		if (key == platform::keys::F4) return shifted(0, 5);
		if (key == platform::keys::F6) return shifted(0, 6);
		if (key == platform::keys::F8) return shifted(0, 3);
		return 0;
	}

	class audio_pacer
	{
	public:
		enum class submit_result
		{
			no_stream,
			not_ready,
			submitted,
			rejected,
		};

		void set_stream(platform::audio_stream_ptr stream)
		{
			_stream = std::move(stream);
		}

		[[nodiscard]] bool can_step() const
		{
			return !_stream || _stream->can_write();
		}

		// Checking before generate_audio is important: a full ring must not
		// advance SID's audio clock and then silently lose that frame.
		submit_result submit(machine& c64)
		{
			if (!_stream) return submit_result::no_stream;
			if (!_stream->can_write()) return submit_result::not_ready;

			const int count = c64.generate_audio(_samples.data(),
				static_cast<int>(_samples.size()));
			if (count <= 0) return submit_result::submitted;
			return _stream->write(std::span<const int16_t>(_samples.data(),
				static_cast<size_t>(count)))
				? submit_result::submitted
				: submit_result::rejected;
		}

		void stop()
		{
			if (_stream) _stream->stop();
		}

	private:
		platform::audio_stream_ptr _stream;
		std::vector<int16_t> _samples = std::vector<int16_t>(4096);
	};

	audio_pacer g_audio_pacer;
	platform::audio_stream_ptr g_audio;

	void start_audio()
	{
		std::lock_guard lock(g_machine_mutex);
		if (g_audio || g_headless) return;
		if (!platform::sound_init()) return;

		g_audio = platform::create_audio_stream(kSampleRate, 1, kAudioBlocks);
		if (!g_audio)
		{
			platform::sound_shutdown();
			return;
		}

		g_audio_pacer.set_stream(g_audio);
		g_sound_started = true;
		g_audio_ok.store(true);
		update_title();
	}

	void stop_audio()
	{
		std::lock_guard lock(g_machine_mutex);
		if (g_audio)
		{
			g_audio_pacer.stop();
			g_audio.reset();
			g_audio_pacer.set_stream(nullptr);
		}
		if (g_sound_started)
		{
			platform::sound_shutdown();
			g_sound_started = false;
		}
		g_audio_ok.store(false);
	}

	void reset_audio_clock()
	{
		if (g_audio) g_audio->stop();
		g_machine.set_audio_enabled(true);
	}

	void reset_machine()
	{
		bool was_sid = false;
		{
			std::lock_guard lock(g_machine_mutex);
			was_sid = g_sid_playing.load();
			g_machine.reset();
			reset_audio_clock();
			g_sid_playing.store(false);
			g_media_generation.fetch_add(1);
		}
		if (was_sid) g_media.clear();
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void eject_media()
	{
		{
			std::lock_guard lock(g_machine_mutex);
			g_machine.eject_disk();
			g_machine.eject_crt();
			reset_audio_clock();
			g_sid_playing.store(false);
			g_media_generation.fetch_add(1);
		}
		g_cart_inserted = false;
		g_disk_inserted = false;
		g_media.clear();
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void toggle_pause()
	{
		g_paused.store(!g_paused.load());
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void close_app()
	{
		if (!g_frame) return;
		g_shutting_down.store(true);
		stop_emulation_worker();
		if (!g_headless) save_settings();
		g_frame->close();
	}

	std::string input_mode_name()
	{
		switch (g_arrow_mode.load())
		{
		case arrow_mode::joystick1: return " [Joystick 1]";
		case arrow_mode::joystick2: return " [Joystick 2]";
		default: return " [Keyboard]";
		}
	}

	void update_title()
	{
		if (!g_frame) return;

		std::string title(kProductName);
		title += " - ";
		title += g_media.empty() ? "no media" : g_media;
		title += input_mode_name();
		if (g_paused.load()) title += " - Paused";
		if (!g_audio_ok.load()) title += " (no audio)";
		g_frame->window_text(title);
	}

	int splitter_width()
	{
		return std::max(4, core::mul_div(5, g_dpi, 96));
	}

	int text_scale()
	{
		return std::max(2, core::mul_div(2, g_dpi, 96));
	}

	int default_panel_width()
	{
		return 31 * 8 * text_scale() + 4 * text_scale();
	}

	int panel_width(const int client_width)
	{
		const int max_width = std::max(0, client_width - splitter_width() - kFramebufferW);
		const int min_width = std::min(default_panel_width(), max_width);
		return std::clamp(g_panel_w, min_width, max_width);
	}

	int splitter_x(const int client_width)
	{
		if (!g_show_debug.load()) return -1;
		const int x = client_width - panel_width(client_width) - splitter_width();
		return x >= 0 ? x : -1;
	}

	picture_layout compute_layout()
	{
		picture_layout layout;
		layout.picture_width = g_client.cx;
		layout.splitter = splitter_x(g_client.cx);
		layout.debug = layout.splitter >= 0;
		if (layout.debug) layout.picture_width = layout.splitter;

		if (g_client.cx <= 0 || g_client.cy <= 0 || layout.picture_width <= 0)
			return layout;

		layout.scale = std::max(1, std::min(
			layout.picture_width / kFramebufferW, g_client.cy / kFramebufferH));
		const int render_width = kFramebufferW * layout.scale;
		const int render_height = kFramebufferH * layout.scale;
		layout.x = (layout.picture_width - render_width) / 2;
		layout.y = (g_client.cy - render_height) / 2;
		return layout;
	}

	void fill_rect(const int x, const int y, const int width, const int height,
		const uint32_t colour)
	{
		if (g_surface.pixels.empty() || width <= 0 || height <= 0) return;

		const int left = std::max(0, x);
		const int top = std::max(0, y);
		const int right = std::min(g_surface.width, x + width);
		const int bottom = std::min(g_surface.height, y + height);
		if (right <= left || bottom <= top) return;

		for (int row = top; row < bottom; ++row)
		{
			auto* const begin = g_surface.pixels.data() +
				static_cast<size_t>(row) * g_surface.width + left;
			std::fill(begin, begin + right - left, colour);
		}
	}

	void blit_picture(const uint32_t* source, const int dx, const int dy, const int scale)
	{
		if (!source || g_surface.pixels.empty()) return;

		const int width = kFramebufferW * scale;
		const int height = kFramebufferH * scale;
		const bool fits = dx >= 0 && dy >= 0 &&
			dx + width <= g_surface.width && dy + height <= g_surface.height;
		if (fits)
		{
			const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint32_t);
			for (int y = 0; y < kFramebufferH; ++y)
			{
				auto* const row = g_surface.pixels.data() +
					static_cast<size_t>(dy + y * scale) * g_surface.width + dx;
				for (int x = 0; x < kFramebufferW; ++x)
					std::fill_n(row + static_cast<size_t>(x) * scale, scale, source[x]);
				for (int r = 1; r < scale; ++r)
					std::memcpy(row + static_cast<size_t>(r) * g_surface.width, row, row_bytes);
				source += kFramebufferW;
			}
			return;
		}

		// The platform lets the user resize below the preferred minimum. Keep the
		// compositor safe and retain the same nearest-neighbour mapping there.
		for (int sy = 0; sy < kFramebufferH; ++sy)
		{
			for (int sx = 0; sx < kFramebufferW; ++sx)
			{
				const uint32_t colour = source[sy * kFramebufferW + sx];
				for (int yy = 0; yy < scale; ++yy)
				{
					const int y = dy + sy * scale + yy;
					if (y < 0 || y >= g_surface.height) continue;
					for (int xx = 0; xx < scale; ++xx)
					{
						const int x = dx + sx * scale + xx;
						if (x >= 0 && x < g_surface.width)
							g_surface.pixels[static_cast<size_t>(y) * g_surface.width + x] = colour;
					}
				}
			}
		}
	}

	uint8_t screen_code(const char c)
	{
		const auto value = static_cast<uint8_t>(c);
		if (value >= 0x60 && value < 0x80) return static_cast<uint8_t>(value - 0x60);
		if (value >= 0x40 && value < 0x60) return static_cast<uint8_t>(value - 0x40);
		if (value >= 0x20 && value < 0x40) return value;
		return 0x20;
	}

	class panel_writer
	{
	public:
		panel_writer(const int left, const int top, const int right, const int bottom,
			const int scale, const bool measuring = false)
			: _left(left), _top(top), _right(right), _bottom(bottom), _scale(scale),
			  _y(top), _measuring(measuring)
		{
		}

		int cell() const { return 8 * _scale; }
		int height() const { return _y - _top; }
		int width() const { return _width; }

		void blank() { _y += cell(); }

		void line(const uint32_t colour, const char* format, ...)
		{
			char text[80];
			va_list args;
			va_start(args, format);
			const int written = std::vsnprintf(text, sizeof(text), format, args);
			va_end(args);

			const int chars = std::clamp(written, 0, static_cast<int>(sizeof(text)) - 1);
			_width = std::max(_width, chars * cell());
			if (_measuring)
			{
				_y += cell();
				return;
			}

			if (_y + cell() > _bottom) return;
			int x = _left;
			for (const char* c = text; *c && x + cell() <= _right; ++c, x += cell())
				glyph(x, _y, screen_code(*c), colour);
			_y += cell();
		}

	private:
		void glyph(const int x, const int y, const uint8_t code, const uint32_t colour) const
		{
			const uint8_t* const rows = rom_chars + static_cast<size_t>(code) * 8;
			for (int gy = 0; gy < 8; ++gy)
			{
				const uint8_t bits = rows[gy];
				for (int gx = 0; gx < 8; ++gx)
				{
					if (!(bits & (0x80 >> gx))) continue;
					for (int sy = 0; sy < _scale; ++sy)
					{
						const int py = y + gy * _scale + sy;
						if (py < 0 || py >= g_surface.height) continue;
						for (int sx = 0; sx < _scale; ++sx)
						{
							const int px = x + gx * _scale + sx;
							if (px >= 0 && px < g_surface.width)
								g_surface.pixels[static_cast<size_t>(py) * g_surface.width + px] = colour;
						}
					}
				}
			}
		}

		int _left;
		int _top;
		int _right;
		int _bottom;
		int _scale;
		int _y;
		bool _measuring;
		int _width = 0;
	};

	void emit_debug_lines(panel_writer& writer, const debug_state& state, const int fps)
	{
		const uint32_t head = vic_palette[1];
		const uint32_t text = vic_palette[14];
		const uint32_t mark = vic_palette[7];

		char flags[9];
		static constexpr char flag_names[] = "NV-BDIZC";
		for (int i = 0; i < 8; ++i)
			flags[i] = (state.status & (0x80 >> i)) ? flag_names[i] : '.';
		flags[8] = 0;

		writer.line(head, "CPU");
		writer.line(text, "PC %04X A %02X X %02X Y %02X SP %02X",
			state.pc, state.a, state.x, state.y, state.sp);
		writer.line(text, "P %02X %s  IRQ %c NMI %c", state.status, flags,
			state.irq_pending ? '*' : '.', state.nmi_pending ? '*' : '.');
		writer.line(text, "$01 %02X %s %s %s %s%s", state.port,
			state.basic_on ? "BAS" : "---", state.kernal_on ? "KER" : "---",
			state.io_on ? "I/O" : "---", state.char_on ? "CHR" : "---",
			state.cart_inserted ? " CRT" : "");

		writer.blank();
		writer.line(head, "DISASSEMBLY");
		for (int i = 0; i < state.disasm_count; ++i)
		{
			const disasm_line& line = state.disasm[i];
			char raw[10];
			switch (line.length)
			{
			case 1: std::snprintf(raw, sizeof(raw), "%02X      ", line.bytes[0]); break;
			case 2: std::snprintf(raw, sizeof(raw), "%02X %02X   ",
				line.bytes[0], line.bytes[1]); break;
			default: std::snprintf(raw, sizeof(raw), "%02X %02X %02X",
				line.bytes[0], line.bytes[1], line.bytes[2]); break;
			}
			writer.line(i == state.current ? mark : text, "%c%04X %s %s",
				i == state.current ? '>' : ' ', line.address, raw, line.text);
		}

		writer.blank();
		writer.line(head, "VIC-II");
		writer.line(text, "MODE %-9s RASTER %03u/%03u", state.mode,
			state.raster, state.raster_compare);
		writer.line(text, "D011 %02X D016 %02X D018 %02X",
			state.ctrl1, state.ctrl2, state.mem_ptr);
		writer.line(text, "SCR %04X CHR %04X BMP %04X",
			state.screen_base, state.char_base, state.bitmap_base);
		writer.line(text, "BANK %04X BORDER %X BG %X",
			state.vic_bank, state.border, state.background);
		writer.line(text, "SPR %02X HIT %02X/%02X IRQ %02X/%02X",
			state.sprite_enable, state.sprite_sprite_hit, state.sprite_bg_hit,
			state.vic_irq_status, state.vic_irq_enable);

		writer.blank();
		writer.line(head, "CIA");
		for (int c = 0; c < 2; ++c)
			writer.line(text, "%d A %04X%c B %04X%c ICR %02X/%02X", c + 1,
				state.timer[c * 2], state.timer_running[c * 2] ? '>' : '.',
				state.timer[c * 2 + 1], state.timer_running[c * 2 + 1] ? '>' : '.',
				state.cia_icr[c], state.cia_mask[c]);

		writer.blank();
		writer.line(head, "SID");
		for (int v = 0; v < 3; ++v)
		{
			const uint8_t* const regs = state.sid + v * 7;
			writer.line(text, "%d F%04X W%02X AD%02X SR%02X E%02X", v + 1,
				static_cast<unsigned>(regs[0] | (regs[1] << 8)), regs[4],
				regs[5], regs[6], state.sid_env[v]);
		}
		writer.line(text, "VOL %X FILT %02X CUTOFF %04X", state.sid[0x18] & 0x0F,
			state.sid[0x17], static_cast<unsigned>((state.sid[0x15] & 0x07) |
			(state.sid[0x16] << 3)));

		writer.blank();
		writer.line(head, "HOST");
		if (fps < 0)
			writer.line(text, "EMULATION PAUSED");
		else
			writer.line(fps < 48 ? mark : text, "EMULATION %d FPS (%d%%)", fps, fps * 2);
	}

	int emulated_fps()
	{
		static auto last_time = std::chrono::steady_clock::now();
		static int64_t last_frames = 0;
		static int value = 50;
		static bool was_paused = false;
		const auto now = std::chrono::steady_clock::now();

		if (g_paused.load())
		{
			was_paused = true;
			return -1;
		}
		if (was_paused)
		{
			was_paused = false;
			last_frames = g_emu_frames.load();
			last_time = now;
		}

		const auto elapsed = std::chrono::duration<double>(now - last_time).count();
		if (elapsed >= 0.5)
		{
			const int64_t frames = g_emu_frames.load();
			value = static_cast<int>((frames - last_frames) / elapsed + 0.5);
			last_frames = frames;
			last_time = now;
		}
		return value;
	}

	void draw_debug_panel(const int left, const int top, const int right, const int bottom,
		const debug_state& state)
	{
		const int scale = text_scale();
		panel_writer measure(left, top, right, bottom, scale, true);
		emit_debug_lines(measure, state, emulated_fps());

		const int x = left + std::max(scale, (right - left - measure.width()) / 2);
		const int y = top + std::max(scale, (bottom - top - measure.height()) / 2);
		panel_writer draw(x, y, right, bottom, scale);
		emit_debug_lines(draw, state, emulated_fps());
	}

	bool compose()
	{
		if (g_client.cx <= 0 || g_client.cy <= 0) return false;

		{
			std::lock_guard lock(g_frame_mutex);
			if (g_frame_fresh)
			{
				std::swap(g_frame_ready, g_frame_read);
				g_frame_fresh = false;
			}
		}
		const frame_snapshot& current = g_frames[g_frame_read];
		const picture_layout layout = compute_layout();
		if (layout != g_layout ||
			g_surface.pixels.size() != static_cast<size_t>(g_client.cx) * g_client.cy)
		{
			g_layout = layout;
			g_surface.width = g_client.cx;
			g_surface.height = g_client.cy;
			g_surface.pixels.assign(static_cast<size_t>(g_client.cx) * g_client.cy, 0);
		}
		else
		{
			std::fill(g_surface.pixels.begin(), g_surface.pixels.end(), 0);
		}

		fill_rect(0, 0, g_layout.picture_width, g_layout.y, 0);
		fill_rect(0, g_layout.y + kFramebufferH * g_layout.scale,
			g_layout.picture_width, g_client.cy, 0);
		fill_rect(0, g_layout.y, g_layout.x, kFramebufferH * g_layout.scale, 0);
		fill_rect(g_layout.x + kFramebufferW * g_layout.scale, g_layout.y,
			g_layout.picture_width, kFramebufferH * g_layout.scale, 0);
		blit_picture(current.pixels.data(), g_layout.x, g_layout.y, g_layout.scale);

		if (g_layout.debug)
		{
			fill_rect(g_layout.splitter, 0, splitter_width(), g_client.cy, vic_palette[11]);
			const int panel_x = g_layout.splitter + splitter_width();
			fill_rect(panel_x, 0, g_client.cx - panel_x, g_client.cy, vic_palette[6]);
			draw_debug_panel(panel_x, 0, g_client.cx, g_client.cy, current.debug);
		}
		return true;
	}

	bool present()
	{
		return compose();
	}

	void sleep_ms(const int milliseconds)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	}

	void request_ui_tick()
	{
		if (g_ui_tick_pending.exchange(true)) return;
		platform::run_ui([]
		{
			g_ui_tick_pending.store(false);
			if (g_shutting_down.load()) return;

			if (g_sid_stopped_pending.exchange(false) &&
				g_sid_stopped_generation.load() == g_media_generation.load())
			{
				g_media.clear();
				update_title();
			}
			if (g_audio_failure_pending.exchange(false))
				update_title();
			if (g_frame) g_frame->invalidate();
		});
	}

	void audio_fallback_after_rejection()
	{
		if (!g_audio_ok.exchange(false)) return;
		g_audio_pacer.stop();
		g_audio_failure_pending.store(true);
		std::fprintf(stderr, "lib8bit: audio stream rejected a PCM block; falling back to wall-clock pacing\n");
	}

	void emulation_worker()
	{
		auto next_frame = platform::monotonic_seconds();

		while (!g_worker_stop.load())
		{
			if (g_paused.load())
			{
				sleep_ms(10);
				next_frame = platform::monotonic_seconds();
				continue;
			}

			const bool audio = g_audio_ok.load();
			if (audio)
			{
				bool can_step = false;
				{
					std::lock_guard lock(g_machine_mutex);
					can_step = g_audio_pacer.can_step();
				}
				if (!can_step)
				{
					sleep_ms(1);
					continue;
				}
			}
			else if (!g_headless)
			{
				constexpr double period = 1.0 / 50.0;
				const double now = platform::monotonic_seconds();
				if (now - next_frame < period)
				{
					sleep_ms(1);
					continue;
				}
				next_frame += period;
				if (now - next_frame > period * 4) next_frame = now;
			}

			frame_snapshot& target = g_frames[g_frame_write];
			audio_pacer::submit_result audio_result = audio_pacer::submit_result::no_stream;
			bool sid_stopped = false;
			{
				std::lock_guard lock(g_machine_mutex);
				if (g_worker_stop.load()) break;

				apply_joystick();
				g_machine.exec(kCyclesPerFrame);
				if (audio)
				{
					audio_result = g_audio_pacer.submit(g_machine);
					if (audio_result == audio_pacer::submit_result::not_ready ||
						audio_result == audio_pacer::submit_result::no_stream)
					{
						// The pre-flight check and write are intentionally separate
						// public operations. If capacity changed between them,
						// consume this frame's SID span before switching clocks.
						g_machine.generate_audio(g_audio_scratch.data(),
							static_cast<int>(g_audio_scratch.size()));
					}
				}
				else
				{
					g_machine.generate_audio(g_audio_scratch.data(),
						static_cast<int>(g_audio_scratch.size()));
					audio_result = audio_pacer::submit_result::submitted;
				}

				if (audio && audio_result != audio_pacer::submit_result::submitted)
					audio_fallback_after_rejection();

				std::memcpy(target.pixels.data(), g_machine.framebuffer(),
					target.pixels.size() * sizeof(uint32_t));
				if (g_show_debug.load()) g_machine.capture_debug(target.debug);
				if (g_sid_playing.load() && !g_machine.sid_player_active())
				{
					g_sid_playing.store(false);
					g_sid_stopped_generation.store(g_media_generation.load());
					sid_stopped = true;
				}
			}

			{
				std::lock_guard lock(g_frame_mutex);
				std::swap(g_frame_write, g_frame_ready);
				g_frame_fresh = true;
			}
			g_emu_frames.fetch_add(1);
			if (sid_stopped) g_sid_stopped_pending.store(true);
			request_ui_tick();
			if (!audio && g_headless) sleep_ms(1);
		}

		g_worker_active.store(false);
	}

	bool start_emulation_worker()
	{
		if (g_emu_executor) return true;

		g_worker_stop.store(false);
		g_worker_active.store(true);
		g_emu_executor = platform::create_serial_executor("lib8bit-emulator");
		if (!g_emu_executor || !g_emu_executor->post(emulation_worker))
		{
			std::fprintf(stderr, "lib8bit: could not start the emulation worker\n");
			if (g_emu_executor) g_emu_executor->stop();
			g_emu_executor.reset();
			g_worker_active.store(false);
			return false;
		}
		return true;
	}

	void stop_emulation_worker()
	{
		g_worker_stop.store(true);
		if (g_emu_executor)
		{
			g_emu_executor->stop();
			g_emu_executor.reset();
		}
		g_worker_active.store(false);
	}

	class screen_element final : public ui::element
	{
		mutable render::texture_ptr _texture;

	public:
		screen_element() : ui::element(ui::element_options(ui::flex_item::grow | ui::flex_item::stretch))
		{
		}

		core::sizei measure(render::measure_context&, const int) const override
		{
			return {kFramebufferW + splitter_width(), kFramebufferH};
		}

		void reset_graphics()
		{
			_texture.reset();
			g_texture_surface.reset();
		}

		void render(render::draw_context& dc, const core::pointi element_offset) const override
		{
			const auto destination = bounds.offset(element_offset);
			if (!compose())
			{
				dc.draw_rect(destination, core::color(core::rgba(0, 0, 0)));
				return;
			}

			if (!g_texture_surface || g_texture_surface->dimensions() != core::sizei{g_surface.width, g_surface.height})
			{
				g_texture_surface = std::make_shared<core::surface>();
				g_texture_surface->alloc({g_surface.width, g_surface.height}, core::texture_format::RGB);
			}

			for (int y = 0; y < g_surface.height; ++y)
			{
				std::memcpy(g_texture_surface->pixels_line(y),
					g_surface.pixels.data() + static_cast<size_t>(y) * g_surface.width,
					static_cast<size_t>(g_surface.width) * sizeof(uint32_t));
			}

			if (!_texture) _texture = dc.create_texture();
			if (_texture)
			{
				_texture->update(g_texture_surface);
				dc.draw_texture(_texture, destination, 1.0f, render::texture_sampler::point);
			}
		}
	};

	void release_held_key(const char32_t key)
	{
		for (size_t i = 0; i < g_held_keys.size(); ++i)
		{
			if (g_held_keys[i].key != key) continue;
			const auto held = g_held_keys[i];
			g_held_keys.erase(g_held_keys.begin() + static_cast<ptrdiff_t>(i));
			send_keys(held.positions.data(), held.count, false);
			return;
		}
	}

	void release_joystick_key(const char32_t key)
	{
		uint8_t bit = 0;
		if (key == platform::keys::UP) bit = kJoyUp;
		else if (key == platform::keys::DOWN) bit = kJoyDown;
		else if (key == platform::keys::LEFT) bit = kJoyLeft;
		else if (key == platform::keys::RIGHT) bit = kJoyRight;
		else if (key == platform::keys::SPACE) bit = kJoyFire;
		if (bit) g_joystick.fetch_and(static_cast<uint8_t>(~bit));
	}

	void sync_held_keys()
	{
		std::vector<char32_t> released;
		for (const auto& held : g_held_keys)
		{
			if (!platform::is_key_down(held.key)) released.emplace_back(held.key);
		}
		for (const auto key : released) release_held_key(key);

		if (!platform::is_key_down(platform::keys::PRIOR)) g_restore_sent.store(false);

		if (g_arrow_mode.load() != arrow_mode::cursor)
		{
			if (!platform::is_key_down(platform::keys::UP)) release_joystick_key(platform::keys::UP);
			if (!platform::is_key_down(platform::keys::DOWN)) release_joystick_key(platform::keys::DOWN);
			if (!platform::is_key_down(platform::keys::LEFT)) release_joystick_key(platform::keys::LEFT);
			if (!platform::is_key_down(platform::keys::RIGHT)) release_joystick_key(platform::keys::RIGHT);
			if (!platform::is_key_down(platform::keys::SPACE)) release_joystick_key(platform::keys::SPACE);
		}
	}

	ui::command_ptr make_command(const std::string_view text, std::function<void()> invoke = {},
		const bool checked = false, const bool enabled = true, const char32_t key = 0,
		const uint32_t key_state = 0)
	{
		auto result = std::make_shared<ui::command>();
		result->text = text;
		result->invoke = std::move(invoke);
		result->checked = checked;
		result->checkable = checked;
		result->enable = enabled;
		if (key != 0)
		{
			result->kba.push_back({key, key_state});
			result->keyboard_accelerator_text = platform::format_keyboard_accelerator(result->kba);
		}
		return result;
	}

	std::vector<ui::command_ptr> build_menu()
	{
		const auto ctrl = platform::keyboard_accelerator_t::control;
		auto file = std::make_shared<ui::command>();
		file->text = "&File";
		file->menu = [ctrl]
		{
			std::vector<ui::command_ptr> result;
			result.emplace_back(make_command("&Open...", open_dialog, false, true, U'O', ctrl));
			result.emplace_back(make_command("&Pause", toggle_pause, g_paused.load(), true,
				platform::keys::F9, 0));
			result.emplace_back(nullptr);
			result.emplace_back(make_command("&Reset (Keeps Cartridge)", reset_machine, false, true, U'R', ctrl));
			result.emplace_back(make_command("&Eject Media", eject_media, false,
				g_cart_inserted || g_disk_inserted, U'E', ctrl));
			result.emplace_back(nullptr);
			result.emplace_back(make_command("E&xit", close_app));
			return result;
		};

		auto input = std::make_shared<ui::command>();
		input->text = "&Input";
		input->menu = []
		{
			return std::vector<ui::command_ptr>{
				make_command("&Cursor Keys", [] { set_arrow_mode(arrow_mode::cursor); },
					g_arrow_mode.load() == arrow_mode::cursor),
				make_command("Joystick &1", [] { set_arrow_mode(arrow_mode::joystick1); },
					g_arrow_mode.load() == arrow_mode::joystick1),
				make_command("Joystick &2", [] { set_arrow_mode(arrow_mode::joystick2); },
					g_arrow_mode.load() == arrow_mode::joystick2),
			};
		};

		auto view = std::make_shared<ui::command>();
		view->text = "&View";
		view->menu = []
		{
			return std::vector<ui::command_ptr>{make_command("&Debug Panel", []
			{
				g_show_debug.store(!g_show_debug.load());
				if (g_frame) g_frame->invalidate();
			}, g_show_debug.load(), true, platform::keys::F11)};
		};

		auto help = std::make_shared<ui::command>();
		help->text = "&Help";
		help->menu = []
		{
			std::vector<ui::command_ptr> result;
			result.emplace_back(make_command("&Keyboard...", []
			{
				platform::message_box(nullptr,
					"Emulated C64 keys\n\n"
					"Keys are mapped by position, so Shift types the C64's own shifted symbols.\n\n"
					"Esc\tRUN/STOP\n"
					"Page Up\tRESTORE (with Esc held: RUN/STOP+RESTORE)\n"
					"Tab\tCommodore key\n"
					"` (backtick)\tLeft arrow\n"
					"Home\tCLR/HOME\n"
					"Backspace, Delete\tINST/DEL\n"
					"' (quote)\t:\n"
					"[  ]\t@  *\n"
					"\\ (backslash)\tUp arrow\n"
					"Page Down\t=\n"
					"End\tpound\n"
					"F1 - F8\tC64 function keys\n"
					"Arrow keys\tCursor keys, or the Input menu's joystick\n"
					"Space\tSpace bar, and joystick fire in joystick mode\n\n"
					"Host shortcuts\n\n"
					"Ctrl+O\tOpen a file\n"
					"Ctrl+R\tReset (the cartridge stays inserted)\n"
					"Ctrl+E\tEject media\n"
					"F9\tPause / resume\n"
					"F11\tShow / hide the debug panel\n"
					"F12\tAbout",
					kProductName, {platform::message_box_buttons::ok, platform::message_box_icon::info});
			}, false, true));
			result.emplace_back(make_command("&About...", []
			{
				platform::message_box(nullptr,
					"lib8bit\n\nA Commodore 64 emulator.\n\n"
					"Open or drop a .PRG, .CRT, .SID, .D64, .D71 or .D81 file, "
					"or pass one on the command line.\n"
					"A .SID tune opens in a player screen: 1-9 select a sub-tune "
					"and X stops playback.\n"
					"Use the Input menu to point the arrow keys at the C64 cursor "
					"keys or at joystick port 1 or 2.",
					kProductName, {platform::message_box_buttons::ok, platform::message_box_icon::info});
			}, false, true, platform::keys::F12));
			return result;
		};

		return {std::move(file), std::move(input), std::move(view), std::move(help)};
	}

	class lib8bit_app final : public ui::app, public ui::frame_host,
		public std::enable_shared_from_this<lib8bit_app>
	{
		std::shared_ptr<ui::elements> _root;
		ui::control_ptr _menu_bar;
		bool _laid_out = false;
		bool _positioned = false;

		core::pointi content_location(const core::pointi loc) const
		{
			return g_screen ? loc - g_screen->bounds.top_left() : loc;
		}

		bool is_splitter(const core::pointi loc) const
		{
			const auto content = content_location(loc);
			return g_layout.splitter >= 0 && content.x >= g_layout.splitter &&
				content.x < g_layout.splitter + splitter_width();
		}

		bool dispatch_c64_key(const char32_t key, const platform::key_state keys)
		{
			if (key == platform::keys::PRIOR)
			{
				if (!keys.repeat && !g_restore_sent.exchange(true))
				{
					std::lock_guard lock(g_machine_mutex);
					g_machine.press_restore();
				}
				return true;
			}

			if (g_arrow_mode.load() != arrow_mode::cursor)
			{
				uint8_t bit = 0;
				if (key == platform::keys::UP) bit = kJoyUp;
				else if (key == platform::keys::DOWN) bit = kJoyDown;
				else if (key == platform::keys::LEFT) bit = kJoyLeft;
				else if (key == platform::keys::RIGHT) bit = kJoyRight;
				else if (key == platform::keys::SPACE) bit = kJoyFire;

				if (bit)
				{
					g_joystick.fetch_or(bit);
					if (!g_paused.load()) g_joystick_taps.fetch_or(bit);
					if (key != platform::keys::SPACE) return true;
				}
			}

			keypos positions[2];
			const auto count = map_key(static_cast<unsigned int>(key), positions);
			if (count <= 0) return false;
			for (const auto& held : g_held_keys)
				if (held.key == key) return true;

			held_key held;
			held.key = key;
			held.count = count;
			for (int i = 0; i < count; ++i) held.positions[static_cast<size_t>(i)] = positions[i];
			g_held_keys.emplace_back(held);
			send_keys(positions, count, true);
			return true;
		}

	public:
		bool wants_gpu() const override
		{
			return !g_headless;
		}

		bool key_down(const char32_t key, const platform::key_state keys) override
		{
			// Alt combinations belong to the native window/menu handling, not the C64 matrix.
			if (keys.alt) return false;
			if (ui::dispatch_accelerator(g_menus, key, keys)) return true;
			return dispatch_c64_key(key, keys);
		}

		bool text_input(std::string_view) override
		{
			return false;
		}

		ui::focus_mode focus_mode() const override
		{
			return ui::focus_mode::none;
		}

		void track_menu(const ui::frame_ptr& parent, const core::recti bounds,
			const std::vector<ui::command_ptr>& commands) override
		{
			if (parent) parent->track_menu(bounds, commands);
		}

		bool load_settings(const platform::setting_file_ptr&) override
		{
			// The host keeps the historical roaming INI path rather than the process-wide store.
			return true;
		}

		bool parse_command_line(const std::string_view command_line) override
		{
			g_headless = false;
			g_start_path.clear();
			for (const auto part : core::split(core::trim(command_line), true, core::is_white_space))
			{
				if (part.empty()) continue;
				if (core::icmp(part, "-test") == 0 || core::icmp(part, "/test") == 0)
				{
					g_headless = true;
				}
				else if (g_start_path.is_empty() && part[0] != '-')
				{
					g_start_path = core::file_path(part);
				}
			}
			// -test is an offscreen UI run, not platform's console-only mode.
			return false;
		}

		int run_console_mode() override
		{
			return 0;
		}

		bool init(const std::string_view) override
		{
			if (!g_headless)
			{
				configure_settings();
				load_ui_settings();
			}
			else
			{
				g_settings_available = false;
				g_settings_store.reset();
			}

			g_menus = build_menu();
			_menu_bar = ui::make_menu_bar(g_menus);
			g_screen = std::make_shared<screen_element>();
			_root = std::make_shared<ui::elements>();
			_root->flex_container.direction = ui::flex_direction::column;
			_root->flex_container.wrap = ui::flex_wrap::no_wrap;
			_root->flex_container.align_items = ui::flex_align::stretch;
			_root->add(_menu_bar);
			_root->add(g_screen);

			const auto initial_dpi = std::max(96, platform::primary_screen_dpi());
			g_dpi = initial_dpi;
			const auto initial_scale = initial_dpi / 96.0;
			const auto initial_menu_reserve = core::round(32 * initial_scale);
			const auto initial_panel = g_headless || g_settings.show_debug
				? default_panel_width() + splitter_width() : 0;
			g_minimum_client_size = {kFramebufferW + splitter_width(), kFramebufferH + initial_menu_reserve};

			ui::frame_style style;
			style.hardware_accelerated = wants_gpu();
			style.drawn_caption = false;
			style.can_focus = true;
			style.can_drop = true;
			style.offscreen = g_headless;
			style.resizable = true;
			style.colors = {core::rgba(0, 0, 0), core::rgba(255, 255, 255), core::rgba(64, 64, 64)};
			style.client_size = {
				core::round(kFramebufferW * 2 * initial_scale) + initial_panel + core::round(96 * initial_scale),
				core::round(kFramebufferH * 2 * initial_scale) + core::round(80 * initial_scale) + initial_menu_reserve
			};
			style.minimum_client = g_minimum_client_size;

			// Placement is deliberately restored from the old keys below; passing the store here would
			// let the frame prefer its newer opaque window_rect value instead.
			g_frame = ui::create_app_frame(shared_from_this(), nullptr, shared_from_this(), style);
			if (!g_frame) return false;
			g_frame->window_text(kProductName);
			if (g_settings.has_placement)
			{
				g_frame->window_bounds(g_settings.placement, true);
				if (g_settings.maximized) g_frame->sys_command(platform::sys_command_type::MAXIMIZE);
				_positioned = true;
			}
			else
			{
				// The native window can lay out before create_app_frame returns; force one pass after
				// g_frame is assigned so the measured menu height also drives default centring.
				g_frame->layout();
			}

			g_machine.set_render_enabled(true);
			g_machine.exec(3'000'000);
			g_machine.set_audio_enabled(true);
			g_running.store(true);
			g_worker_stop.store(false);
			g_shutting_down.store(false);
			if (!g_start_path.is_empty())
			{
				const auto path = g_start_path;
				g_start_path.clear();
				load_file(path);
			}
			return true;
		}

		bool owns_loop() const override
		{
			return true;
		}

		int main_loop() override;

		void idle() override
		{
			sync_held_keys();
		}

		void frame_tick() override
		{
			sync_held_keys();
			if (g_frame) g_frame->invalidate();
		}

		void system_event(const platform::os_event_type event) override
		{
			switch (event)
			{
			case platform::os_event_type::session_ending:
				save_settings();
				break;
			case platform::os_event_type::graphics_device_lost:
				if (g_screen) g_screen->reset_graphics();
				if (g_frame) g_frame->invalidate();
				break;
			case platform::os_event_type::system_suspending:
				stop_audio();
				break;
			case platform::os_event_type::system_resumed:
				if (!g_headless) start_audio();
				break;
			default:
				break;
			}
		}

		bool can_exit() override
		{
			// The close box and taskbar close arrive here without going through a menu command.
			if (!g_headless) save_settings();
			return true;
		}

		void exit() override
		{
			g_shutting_down.store(true);
			stop_emulation_worker();
			stop_audio();
		}

		void final_exit() override
		{
			if (g_screen) g_screen->reset_graphics();
			_root.reset();
			_menu_bar.reset();
			g_menus.clear();
			g_frame.reset();
			g_settings_store.reset();
		}

		void on_window_layout(render::measure_context& mc, const core::sizei extent,
			const bool is_minimized) override
		{
			if (is_minimized || !_root || !g_screen) return;
			const auto old_dpi = g_dpi;
			g_dpi = std::max(96, static_cast<int>(mc.scale_factor * 96.0 + 0.5));
			if (!_laid_out)
			{
				g_panel_w = g_settings.panel_w96 > 0
					? core::mul_div(static_cast<int>(g_settings.panel_w96), g_dpi, 96)
					: default_panel_width();
				g_show_debug.store(g_headless || g_settings.show_debug);
				_laid_out = true;
			}
			else if (old_dpi != g_dpi)
			{
				g_panel_w = core::mul_div(g_panel_w, g_dpi, std::max(1, old_dpi));
			}

			_root->layout(mc, core::recti(extent));
			g_menu_height = _menu_bar ? _menu_bar->bounds.height() : 0;
			g_client = g_screen->bounds.extent();

			if (!_positioned && !g_headless && g_frame)
			{
				const auto scale = g_frame->scale_factor();
				const auto panel = g_show_debug.load() ? g_panel_w + splitter_width() : 0;
				const auto width = core::round(kFramebufferW * 2 * scale) + panel + core::round(96 * scale);
				const auto height = core::round(kFramebufferH * 2 * scale) + g_menu_height + core::round(80 * scale);
				const auto screen = platform::primary_screen_size();
				const auto left = std::max(0, (screen.cx - width) / 2);
				const auto top = std::max(0, (screen.cy - height) / 2);
				g_frame->window_bounds({left, top, left + width, top + height}, true);
				_positioned = true;
			}
		}

		void on_window_paint(render::draw_context& dc) override
		{
			if (_root) _root->render(dc, {});
		}

		void on_window_destroy() override
		{
			g_shutting_down.store(true);
			stop_emulation_worker();
			stop_audio();
			g_running.store(false);
		}

		void focus_changed(const bool has_focus, const ui::control_ptr&) override
		{
			if (!has_focus)
			{
				g_dragging = false;
				release_all_keys();
			}
		}

		void on_mouse_move(const core::pointi loc, const bool is_tracking) override
		{
			if (g_dragging && !is_tracking) g_dragging = false;
			if (g_dragging)
			{
				g_panel_w = g_client.cx - content_location(loc).x - splitter_width();
				g_panel_w = panel_width(g_client.cx);
				if (g_frame) g_frame->invalidate();
			}
			if (g_frame) g_frame->set_cursor(is_splitter(loc) || g_dragging
				? platform::cursor::left_right : platform::cursor::normal);
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state) override
		{
			if (is_splitter(loc))
			{
				g_dragging = true;
				if (g_frame) g_frame->set_cursor(platform::cursor::left_right);
			}
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state) override
		{
			g_dragging = false;
			if (g_frame) g_frame->set_cursor(is_splitter(loc) ? platform::cursor::left_right : platform::cursor::normal);
		}

		void on_mouse_leave(const core::pointi) override
		{
			g_dragging = false;
			if (g_frame) g_frame->set_cursor(platform::cursor::normal);
		}

		platform::drop_effect drag_over(const platform::clipboard_data& data,
			const platform::key_state, const core::pointi) override
		{
			return data.has_drop_files() ? platform::drop_effect::copy : platform::drop_effect::none;
		}

		platform::drop_effect drag_drop(platform::clipboard_data& data,
			const platform::key_state, const core::pointi) override
		{
			if (!data.has_drop_files()) return platform::drop_effect::none;
			const auto paths = data.drop_paths();
			if (!paths.empty()) load_file(paths.front());
			return platform::drop_effect::copy;
		}

		void visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit) override
		{
			if (_root) visit(_root, {});
		}

		int run_headless();
	};

	class recording_texture final : public render::texture
	{
		bool _valid = false;

	public:
		render::texture_update_result update(const render::video_frame_ptr&) override
		{
			return render::texture_update_result::failed;
		}

		render::texture_update_result update(const core::const_surface_ptr& surface) override
		{
			if (!surface) return render::texture_update_result::failed;
			_dimensions = surface->dimensions();
			_format = surface->format();
			_valid = true;
			return render::texture_update_result::tex_updated;
		}

		render::texture_update_result update(core::sizei dims, core::texture_format format,
			core::orientation orientation, const uint8_t*, size_t, size_t) override
		{
			_dimensions = dims;
			_format = format;
			_orientation = orientation;
			_valid = true;
			return render::texture_update_result::tex_updated;
		}

		bool is_valid() const override
		{
			return _valid;
		}
	};

	class recording_draw_context final : public ui::test::fake_draw_context
	{
		std::shared_ptr<recording_texture> _recording_texture = std::make_shared<recording_texture>();

	public:
		render::texture_ptr drawn_texture;
		core::recti drawn_bounds;

		recording_draw_context(const core::recti clip) : ui::test::fake_draw_context(clip)
		{
		}

		render::texture_ptr create_texture() override
		{
			return _recording_texture;
		}

		void draw_texture(const render::texture_ptr& texture, const core::recti dst, float,
			render::texture_sampler) override
		{
			drawn_texture = texture;
			drawn_bounds = dst;
		}
	};

	class fake_audio_stream final : public platform::audio_stream
	{
		int _capacity;
		int _queued = 0;
		bool _reject_next = false;
		size_t _last_block_size = 0;

	public:
		explicit fake_audio_stream(const int capacity) : _capacity(capacity)
		{
		}

		int queued_blocks() const override { return _queued; }
		bool can_write() const override { return _queued < _capacity; }

		bool write(const std::span<const int16_t> samples) override
		{
			if (!can_write() || samples.empty()) return false;
			if (_reject_next)
			{
				_reject_next = false;
				return false;
			}
			++_queued;
			_last_block_size = samples.size();
			return true;
		}

		void set_volume(float) override
		{
		}

		void stop() override
		{
			_queued = 0;
			_last_block_size = 0;
		}

		void drain_one()
		{
			if (_queued > 0) --_queued;
		}

		void reject_next_write() { _reject_next = true; }
		size_t last_block_size() const { return _last_block_size; }
	};

	bool check_screen_key(const int row, const int col, const bool pressed)
	{
		std::lock_guard lock(g_machine_mutex);
		const uint8_t bit = static_cast<uint8_t>(1u << col);
		return pressed ? (g_machine._state->key_matrix[row] & bit) == 0
			: (g_machine._state->key_matrix[row] & bit) != 0;
	}

	bool wait_for_frame(const int64_t target)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (g_running.load() && g_emu_frames.load() < target &&
			std::chrono::steady_clock::now() < deadline)
		{
			if (!platform::pump_events()) return false;
			platform::pump_ui_tasks(0);
			if (g_frame) g_frame->redraw_now();
			sleep_ms(1);
		}
		return g_emu_frames.load() >= target;
	}

	// A frame already running when the input changed read the old input, so a check waits for the
	// end of one that started after the change.
	bool wait_for_next_frame()
	{
		return wait_for_frame(g_emu_frames.load() + 2);
	}

	int lib8bit_app::run_headless()
	{
		platform::attach_console();
		int failures = 0;
		const auto check = [&](const bool ok, const std::string_view what)
		{
			platform::write_stdout(std::string(ok ? "PASS " : "FAIL ") + std::string(what) + "\n");
			if (!ok) ++failures;
		};

		if (!start_emulation_worker()) check(false, "offscreen emulation worker starts");
		const int64_t initial_frames = g_emu_frames.load();
		check(wait_for_frame(initial_frames + 120), "background worker publishes PAL frames");
		for (int i = 0; i < 4 && g_running.load(); ++i)
		{
			if (!platform::pump_events()) break;
			platform::pump_ui_tasks(0);
			if (g_frame) g_frame->redraw_now();
			sleep_ms(1);
		}

		check(g_client.cx > 0 && g_client.cy > 0, "offscreen window reports a client area");
		check(!g_settings_available, "headless test leaves user settings untouched");
		check(g_minimum_client_size.cx == kFramebufferW + splitter_width() &&
			g_minimum_client_size.cy >= kFramebufferH + g_menu_height,
			"window minimum preserves framebuffer, splitter and menu bar");
		check(g_layout.scale >= 1 && g_layout.scale == static_cast<int>(g_layout.scale),
			"picture layout uses an integer scale");
		check(g_layout.x >= 0 && g_layout.y >= 0, "picture is letterboxed inside the client");
		check(g_surface.width == g_client.cx && g_surface.height == g_client.cy &&
			g_surface.pixels.size() == static_cast<size_t>(g_client.cx) * g_client.cy,
			"surface covers the client area");
		check(std::any_of(g_surface.pixels.begin(), g_surface.pixels.end(),
			[](const uint32_t pixel) { return pixel != 0; }),
			"framebuffer and debug panel reach the surface");

		if (g_screen) g_screen->reset_graphics();
		recording_draw_context draw{core::recti(g_client)};
		on_window_paint(draw);
		check(draw.drawn_texture && draw.drawn_bounds.extent() == g_client,
			"repaint uses render::draw_context");
		const auto recorded_texture = std::dynamic_pointer_cast<recording_texture>(draw.drawn_texture);
		check(recorded_texture && recorded_texture->format() == core::texture_format::RGB &&
			g_texture_surface && g_texture_surface->format() == core::texture_format::RGB,
			"screen texture uses opaque RGB pixels");

		g_end_session_saved = false;
		system_event(platform::os_event_type::session_ending);
		check(g_end_session_saved, "end-session callback saves window settings");

		set_arrow_mode(arrow_mode::cursor);
		check(key_down(U'A', {}), "keyboard app accepts a C64 matrix key");
		wait_for_next_frame();
		check(check_screen_key(1, 2, true), "keyboard app queues a C64 matrix press");
		sync_held_keys();
		wait_for_next_frame();
		check(check_screen_key(1, 2, false), "platform key state releases a C64 matrix key");

		check(key_down(platform::keys::F1, {}), "F1 reaches the C64 key map");
		wait_for_next_frame();
		check(check_screen_key(0, 4, true), "F1 presses the C64 F1 matrix key");
		sync_held_keys();
		wait_for_next_frame();
		check(check_screen_key(0, 4, false), "platform key state releases the C64 F1 key");

		g_paused.store(true);
		check(key_down(platform::keys::PRIOR, {}), "Page Up reaches the app key handler");
		{
			std::lock_guard lock(g_machine_mutex);
			g_machine.flush_keys();
			check(g_machine._state->nmi_pending, "Page Up pulses RESTORE through the app");
		}
		g_paused.store(false);

		set_arrow_mode(arrow_mode::joystick1);
		check(key_down(platform::keys::UP, {}), "joystick direction reaches the app");
		wait_for_next_frame();
		{
			std::lock_guard lock(g_machine_mutex);
			check((g_machine._state->joystick1 & kJoyUp) == 0,
				"joystick mode preserves a short directional tap");
		}
		sync_held_keys();
		wait_for_next_frame();
		{
			std::lock_guard lock(g_machine_mutex);
			check((g_machine._state->joystick1 & kJoyUp) != 0,
				"joystick release reaches the emulated port");
		}
		set_arrow_mode(arrow_mode::cursor);

		auto fake = std::make_shared<fake_audio_stream>(1);
		audio_pacer adapter;
		adapter.set_stream(fake);
		machine audio_machine;
		audio_machine.set_audio_enabled(true);
		audio_machine.exec(kCyclesPerFrame);
		check(adapter.can_step(), "audio adapter reports room in the stream");
		check(adapter.submit(audio_machine) == audio_pacer::submit_result::submitted &&
			fake->last_block_size() > 0, "audio adapter writes one generated PCM block");
		const int64_t audio_cycle = audio_machine._state->sid.audio_cycle;
		check(!adapter.can_step() &&
			adapter.submit(audio_machine) == audio_pacer::submit_result::not_ready &&
			audio_machine._state->sid.audio_cycle == audio_cycle,
			"full audio ring does not advance the SID clock");
		fake->drain_one();
		check(adapter.can_step(), "audio adapter resumes after a block drains");
		audio_machine.exec(kCyclesPerFrame);
		const int64_t rejected_cycle = audio_machine._state->sid.audio_cycle;
		fake->reject_next_write();
		check(adapter.submit(audio_machine) == audio_pacer::submit_result::rejected &&
			audio_machine._state->sid.audio_cycle > rejected_cycle,
			"audio adapter reports a write refused after can_write");

		if (g_layout.splitter >= 0)
		{
			const int old_panel = g_panel_w;
			const auto down = core::pointi{g_layout.splitter + 1 + g_screen->bounds.left, g_screen->bounds.top + 4};
			on_mouse_left_button_down(down, {});
			auto move = core::pointi{std::max(0, g_layout.splitter - 16) + g_screen->bounds.left, g_screen->bounds.top + 4};
			on_mouse_move(move, true);
			on_mouse_left_button_up(move, {});
			check(g_panel_w != old_panel, "debug splitter resizes through ui mouse input");

			const int captured_panel = g_panel_w;
			on_mouse_left_button_down(down, {});
			move.x = std::max(0, g_layout.splitter - 32) + g_screen->bounds.left;
			on_mouse_move(move, false);
			check(g_panel_w == captured_panel, "capture loss clears splitter dragging");
		}

		close_app();
		check(!g_running.load(), "close request ends the offscreen app loop");
		platform::write_stdout("lib8bit app headless: " + std::string(failures == 0 ? "PASS" : "FAIL") + "\n");
		return failures == 0 ? 0 : 1;
	}

	int lib8bit_app::main_loop()
	{
		if (!g_headless) start_audio();
		if (!start_emulation_worker())
		{
			report("Failed to start the emulation worker.");
			if (g_frame) g_frame->close();
			return 1;
		}
		update_title();

		if (g_headless) return run_headless();

		while (g_running.load() && platform::pump_events())
		{
			platform::pump_ui_tasks(0);
			sync_held_keys();
			sleep_ms(1);
		}
		g_running.store(false);
		stop_emulation_worker();
		stop_audio();
		return 0;
	}
}

int main(int argc, char* argv[])
{
	core::set_app_name("lib8bit");
	auto app = std::make_shared<lib8bit_app>();
	return platform::run(app, argc, argv);
}
