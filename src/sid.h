#pragma once

#include <cstdint>
#include <vector>

// lib64 by Zac Walker
//
// MOS6581 / MOS8580 SID sound-chip emulation. Three voices (waveform + ADSR
// envelope) feed a two-integrator analog filter model and the C64's output
// stage, clocked once per CPU cycle, then decimated to the host's sample rate
// by a two-stage anti-aliasing resampler into 16-bit PCM.
//
// Note: the waveform and envelope DACs are taken as linear; the 6581's
// kinked-DAC nonlinearity is not modelled.

enum class sid_model
{
	mos6581,
	mos8580
};

// Builds the shared 4096-entry waveform tables and per-model combined-waveform
// pulldown tables (AVERAGE config). Idempotent; called by SID construction.
void sid_init_tables();

class waveform_generator
{
public:
	waveform_generator() { reset(); }

	void set_model(bool is_6581) { is6581 = is_6581; }
	void set_waveform_models(const int16_t (*models)[4096]);
	void set_pulldown_models(const int16_t (*models)[4096]);
	void set_other(waveform_generator* prev, waveform_generator* next)
	{
		prev_v = prev;
		next_v = next;
	}

	void reset();
	void clock();
	uint16_t output();
	void synchronize();

	void write_freq_lo(uint8_t v) { freq = static_cast<uint16_t>((freq & 0xFF00) | v); }
	void write_freq_hi(uint8_t v) { freq = static_cast<uint16_t>(((v << 8) & 0xFF00) | (freq & 0xFF)); }
	void write_pw_lo(uint8_t v) { pw = static_cast<uint16_t>((pw & 0xF00) | v); }
	void write_pw_hi(uint8_t v) { pw = static_cast<uint16_t>(((v << 8) & 0xF00) | (pw & 0x0FF)); }
	void write_control(uint8_t control);

	uint8_t read_osc() const { return static_cast<uint8_t>((osc3 >> 4) & 0xFF); }
	uint32_t read_accumulator() const { return accumulator; }
	uint16_t read_freq() const { return freq; }
	bool read_test() const { return test; }
	bool read_following_voice_sync() const { return next_v ? next_v->sync : false; }

	uint32_t accumulator = 0x555555;
	bool sync = false;

private:
	void shift_phase2(int waveform_new);
	void set_noise_output();
	void set_no_noise_or_noise_output() { no_noise_or_noise_output = static_cast<uint16_t>(no_noise | noise_output); }
	void write_shift_register();
	void wave_bitfade() { waveform_output &= static_cast<uint16_t>(waveform_output >> 1); }
	void shiftreg_bitfade() { shift_register = (shift_register | 0x400000) & 0x7FFFFF; }

	const int16_t (*model_wave)[4096] = nullptr;
	const int16_t (*model_pulldown)[4096] = nullptr;
	const int16_t* wave = nullptr;
	const int16_t* pulldown = nullptr;

	uint16_t pw = 0;
	uint32_t shift_register = 0x7FFFFF;
	uint32_t shift_latch = 0;
	int shift_pipeline = 0;
	uint32_t ring_msb_mask = 0;
	uint16_t no_noise = 0xFFF;
	uint16_t noise_output = 0;
	uint16_t no_noise_or_noise_output = 0xFFF;
	uint16_t no_pulse = 0xFFF;
	uint16_t pulse_output = 0xFFF;
	int waveform = 0;
	uint16_t waveform_output = 0;
	uint16_t freq = 0;
	uint16_t tri_saw_pipeline = 0x555;
	uint16_t osc3 = 0;
	int shift_register_reset = 0;
	int floating_output_ttl = 0;
	bool test = false;
	bool test_or_reset = false;
	bool msb_rising = false;
	bool is6581 = true;
	waveform_generator* prev_v = nullptr;
	waveform_generator* next_v = nullptr;
};

class envelope_generator
{
public:
	envelope_generator() { reset(); }

	void reset();
	void clock();
	uint8_t output() const { return envelope_counter; }
	uint8_t read_env() const { return env3; }

	void write_control(uint8_t control);
	void write_attack_decay(uint8_t v);
	void write_sustain_release(uint8_t v);

private:
	void state_change();
	void set_exponential_counter();

	enum
	{
		ATTACK = 0,
		DECAY_SUSTAIN = 1,
		RELEASE = 2
	};

	uint16_t lfsr = 0x7FFF;
	uint16_t rate = 0;
	int exponential_counter = 0;
	int exponential_counter_period = 1;
	int new_exponential_counter_period = 0;
	int state_pipeline = 0;
	int envelope_pipeline = 0;
	int exponential_pipeline = 0;
	int state = RELEASE;
	int next_state = RELEASE;
	bool counter_enabled = true;
	bool gate = false;
	bool reset_lfsr = false;
	uint8_t envelope_counter = 0xAA;
	uint8_t attack = 0;
	uint8_t decay = 0;
	uint8_t sustain = 0;
	uint8_t release = 0;
	uint8_t env3 = 0;
};

class voice
{
public:
	waveform_generator wave;
	envelope_generator envelope;

	void reset()
	{
		wave.reset();
		envelope.reset();
	}
	void write_control(uint8_t v)
	{
		wave.write_control(v);
		envelope.write_control(v);
	}
};

// The mixer and the multi-mode filter. A voice's level is its waveform less the
// DAC's zero level, times its envelope; the 6581 adds a DC offset per voice and
// one in the mixer, both scaled by the volume register, which is what makes a
// $D418 write audible as a "digi". The filter is the chip's two-integrator loop,
// whose outputs are inverted, with a cutoff curve per model.
class sid_filter
{
public:
	explicit sid_filter(bool is_6581);

	void set_model(bool is_6581);
	// The clock the chip runs at, which turns a cutoff in hertz into a step per cycle.
	void set_clock_frequency(double hz);
	void reset();
	void input(float v) { ve = v; }
	// One cycle: the voices mixed, filtered and scaled by the volume register.
	// 1.0 is a voice at full envelope swinging to one extreme.
	float clock(voice& v1, voice& v2, voice& v3);

	void write_fc_lo(uint8_t v)
	{
		fc = (fc & 0x7F8) | (v & 0x007);
		update_w0();
	}
	void write_fc_hi(uint8_t v)
	{
		fc = ((v << 3) & 0x7F8) | (fc & 0x007);
		update_w0();
	}
	void write_res_filt(uint8_t v);
	void write_mode_vol(uint8_t v);
	void enable(bool e);
	// Voices (bit 0 = voice 1) left out of the mix. reset() keeps the mask: it
	// is the listener's choice, not chip state. A muted voice still contributes
	// its DC, as a silent voice does, so muting it makes no click.
	void set_mute_mask(uint8_t mask) { mute_mask = mask & 0x07; }
	uint8_t get_mute_mask() const { return mute_mask; }

	// The cutoff, in hertz, of a model's filter at an 11-bit FC value. The 8580's
	// is close to linear, from about 30 Hz to 12.5 kHz; the 6581's is an S-curve
	// from about 230 Hz to 17 kHz, fitted to published measurements of the chip.
	static double cutoff_hz(bool is_6581, int fc);
	// The filter's Q at a resonance setting (0-15).
	static double resonance_q(bool is_6581, int res);

private:
	float voice_output(voice& v, bool muted) const;
	void update_w0();

	bool is6581 = true;
	double clock_hz = 985248;
	double vhp = 0, vbp = 0, vlp = 0;
	float ve = 0;
	int fc = 0;
	int res = 0;
	double w0 = 0;
	double q_inv = 1.0;
	bool filt1 = false, filt2 = false, filt3 = false, filtE = false;
	bool voice3off = false, hp = false, bp = false, lp = false;
	int vol = 0;
	bool enabled = true;
	uint8_t filt = 0;
	uint8_t mute_mask = 0;
	// The waveform DAC's zero level, and the DC each voice and the mixer add, in
	// units of a voice at full envelope. Set by the model.
	float wave_zero = 896.0f;
	float voice_dc = 1.0f;
	float mixer_dc = 0.0f;
};

// The C64's audio output stage: a 16 kHz RC low-pass and the coupling
// capacitor's high-pass at about 16 Hz, which removes the 6581's DC.
class external_filter
{
public:
	void set_clock_frequency(double frequency);
	void reset()
	{
		vlp = 0;
		vhp = 0;
	}
	double clock(double input)
	{
		vlp += w_lp * (input - vlp);
		vhp += w_hp * (vlp - vhp);
		return vlp - vhp;
	}

private:
	double vlp = 0;
	double vhp = 0;
	double w_lp = 0;
	double w_hp = 0;
};

// Decimates the chip's one sample per cycle to the output rate without
// aliasing: a 64-tap FIR takes every eighth cycle, then a polyphase
// windowed-sinc filter interpolates to the output rate. The pass band is flat
// to 20 kHz; what would fold back into it is at least 70 dB down.
class sid_resampler
{
public:
	void configure(double clock_hz, double output_hz);
	void reset();
	// One sample per chip cycle; true, with out set, when an output sample is due.
	bool input(float x, float& out)
	{
		a_ring[a_pos] = x;
		a_ring[a_pos + a_taps] = x;
		if (++a_pos == a_taps) a_pos = 0;
		if (++a_count < decimation) return false;
		a_count = 0;
		return decimated(out);
	}

private:
	static constexpr int decimation = 8;
	static constexpr int a_taps = 64;
	static constexpr int b_taps = 128;
	static constexpr int b_phases = 128;

	bool decimated(float& out);

	float a_coeff[a_taps] = {};
	float a_ring[2 * a_taps] = {};
	int a_pos = 0;
	int a_count = 0;

	std::vector<float> b_table; // (b_phases + 1) rows of b_taps, oldest sample first
	float b_ring[2 * b_taps] = {};
	int b_pos = 0;
	double b_step = 1; // decimated samples per output sample
	double b_acc = 0;
};

class sid_chip
{
public:
	sid_chip();

	void set_chip_model(sid_model model);
	sid_model chip_model() const { return model; }
	// The chip's clock and the output rate. Rebuilds the resampler only when
	// either changes.
	void set_sampling_parameters(double clock_frequency, double sampling_frequency);
	double clock_rate() const { return clock_frequency; }
	double sample_rate() const { return sampling_frequency; }
	void reset();

	uint8_t read(uint16_t offset);
	// Queue a register write timestamped at an absolute CPU cycle.
	void write(uint16_t offset, uint8_t value, int64_t cycle);
	// Apply a register write immediately (used when audio output is disabled).
	void write_now(uint16_t offset, uint8_t value) { apply_register_write(offset & 0x1F, value); }

	// Current envelope level of a voice (0-255), for monitors: the registers are
	// write-only, so this is the only way to see whether a voice is sounding.
	uint8_t envelope_level(int v) const { return voices[v & 3].envelope.output(); }

	// Leave a voice (0-2) out of the output while it keeps running; see sid_filter.
	void set_voice_muted(int v, bool muted);
	bool voice_muted(int v) const;

	// Frame-boundary hook kept for hosts; clock() drains the write queue itself.
	void begin_frame() {}
	// Clock the chip for `cycles`, replaying queued writes at their timestamps,
	// appending up to max_samples 16-bit samples to buf. Returns samples written.
	int clock(int cycles, int16_t* buf, int64_t start_cycle, int max_samples);

	int64_t audio_cycle = 0; // absolute CPU cycle the chip has produced audio up to

private:
	struct queued_write
	{
		uint8_t offset;
		uint8_t value;
		int64_t cycle;
	};

	void apply_register_write(uint8_t offset, uint8_t value);
	void age_bus_value(int n);
	void voice_sync(bool sync);
	int16_t output_sample(float value) const;

	voice voices[3];
	sid_filter filter;
	external_filter ext_filter;
	sid_resampler resampler;

	sid_model model = sid_model::mos6581;
	int model_ttl = 0x01D00;

	uint8_t bus_value = 0;
	int bus_value_ttl = 0;
	int64_t next_voice_sync = 0x7FFFFFFF;

	double clock_frequency = 0;
	double sampling_frequency = 0;

	std::vector<queued_write> write_queue;
};
