// lib64 by Zac Walker
//
// MOS6581 / MOS8580 SID sound-chip emulation.

#include "sid.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// ===========================================================================
// Shared waveform + combined-waveform pulldown tables
// ===========================================================================

namespace
{
	int16_t g_wftable[4][4096];
	int16_t g_pulldown[2][5][4096];

	struct wave_config
	{
		int dist_func; // 0 exponential, 1 linear, 2 quadratic
		double threshold;
		double topbit;
		double pulsestrength;
		double distance1;
		double distance2;
	};

	// Monte-Carlo derived combined-waveform parameters (AVERAGE), [model][waveform],
	// waveform order: TS ($3), PT ($5), PS ($6), PTS ($7), NP ($C).
	const wave_config CONFIG_AVERAGE[2][5] = {
		{
			{0, 0.877322257, 1.11349654, 0, 2.14537621, 9.08618164},
			{1, 0.941692829, 1, 1.80072665, 0.033124879, 0.232303441},
			{1, 1.66494179, 1.03760982, 5.62705326, 0.291590303, 0.283631504},
			{1, 1.09762526, 0.975265801, 1.52196741, 0.151528224, 0.841949463},
			{0, 0.96, 1, 2.5, 1.1, 1.2},
		},
		{
			{0, 0.853578329, 1.09615636, 0, 1.8819375, 6.80794907},
			{0, 0.929835618, 1, 1.12836814, 1.10453653, 1.48065746},
			{2, 0.911938608, 0.996440411, 1.2278074, 0.000117214302, 0.18948476},
			{0, 0.938004673, 1.04827631, 1.21178246, 0.915959001, 1.42698038},
			{0, 0.95, 1, 1.15, 1, 1.45},
		},
	};

	double dist_func(int func, double distance, int i)
	{
		switch (func)
		{
		case 0: return std::pow(distance, -i);            // exponential
		case 1: return 1.0 / (1.0 + i * distance);        // linear
		default: return 1.0 / (1.0 + (i * i) * distance); // quadratic
		}
	}

	int tri_xor(int val)
	{
		return (((val & 0x800) == 0) ? val : (val ^ 0xFFF)) << 1;
	}

	int calculate_pulldown(const double* distance_table, double topbit, double pulsestrength, double threshold,
	                       int accumulator)
	{
		double bit[12];
		for (int i = 0; i < 12; i++) bit[i] = (accumulator & (1 << i)) ? 1.0 : 0.0;
		bit[11] *= topbit;

		double pulldown[12];
		for (int sb = 0; sb < 12; sb++)
		{
			double avg = 0, n = 0;
			for (int cb = 0; cb < 12; cb++)
			{
				if (cb == sb) continue;
				const double weight = distance_table[sb - cb + 12];
				avg += (1 - bit[cb]) * weight;
				n += weight;
			}
			avg -= pulsestrength;
			pulldown[sb] = avg / n;
		}

		int value = 0;
		for (int i = 0; i < 12; i++)
		{
			const double bit_value = bit[i] > 0 ? 1 - pulldown[i] : 0;
			if (bit_value > threshold) value |= 1 << i;
		}
		return value;
	}

	void build_tables()
	{
		for (int idx = 0; idx < 4096; idx++)
		{
			const int saw = idx;
			g_wftable[0][idx] = static_cast<int16_t>(0xFFF);
			g_wftable[1][idx] = static_cast<int16_t>(tri_xor(idx));
			g_wftable[2][idx] = static_cast<int16_t>(saw);
			g_wftable[3][idx] = static_cast<int16_t>(saw & (saw << 1));
		}

		for (int model = 0; model < 2; model++)
		{
			for (int wf = 0; wf < 5; wf++)
			{
				const wave_config& cfg = CONFIG_AVERAGE[model][wf];
				double distance_table[25];
				for (int i = 0; i < 25; i++)
				{
					const double dist = i < 12 ? cfg.distance1 : cfg.distance2;
					distance_table[i] = dist_func(cfg.dist_func, dist, std::abs(i - 12));
				}
				for (int idx = 0; idx < 4096; idx++)
				{
					g_pulldown[model][wf][idx] = static_cast<int16_t>(
						calculate_pulldown(distance_table, cfg.topbit, cfg.pulsestrength, cfg.threshold, idx));
				}
			}
		}
	}
}

void sid_init_tables()
{
	static const bool built = []
	{
		build_tables();
		return true;
	}();
	(void)built;
}

// ===========================================================================
// Waveform generator
// ===========================================================================

namespace
{
	// The five pulldown models cover the combined waveforms TS, PT, PS, PTS and NP.
	const int16_t* select_pulldown(const int16_t (*models)[4096], int waveform)
	{
		if (!models) return nullptr;
		switch (waveform)
		{
		case 0x3: return models[0];
		case 0x5: return models[1];
		case 0x6: return models[2];
		case 0x7: return models[3];
		case 0xC: return models[4];
		default: return nullptr;
		}
	}
}

void waveform_generator::set_waveform_models(const int16_t (*models)[4096])
{
	model_wave = models;
	wave = models ? models[waveform & 3] : nullptr;
}

void waveform_generator::set_pulldown_models(const int16_t (*models)[4096])
{
	model_pulldown = models;
	pulldown = select_pulldown(models, waveform);
}

void waveform_generator::reset()
{
	accumulator = 0x555555;
	freq = 0;
	pw = 0;
	waveform = 0;
	test = false;
	sync = false;
	msb_rising = false;
	shift_register = 0x7FFFFF;
	shift_latch = 0;
	shift_pipeline = 0;
	pulse_output = 0xFFF;
	waveform_output = 0;
	osc3 = 0;
	floating_output_ttl = 0;
	shift_register_reset = 0;
	no_noise = 0xFFF;
	no_pulse = 0xFFF;
	ring_msb_mask = 0;
	test_or_reset = false;
	tri_saw_pipeline = 0x555;
	noise_output = 0;
	no_noise_or_noise_output = no_noise;
	if (model_wave) wave = model_wave[0];
	pulldown = nullptr;
}

void waveform_generator::clock()
{
	if (test)
	{
		if (shift_register_reset != 0 && --shift_register_reset == 0)
		{
			shiftreg_bitfade();
			shift_latch = shift_register;
			set_noise_output();
		}
		test_or_reset = true;
		pulse_output = 0xFFF;
	}
	else
	{
		const uint32_t acc_old = accumulator;
		accumulator = (accumulator + freq) & 0xFFFFFF;
		const uint32_t acc_bits_set = ~acc_old & accumulator;
		msb_rising = (acc_bits_set & 0x800000) != 0;

		if (acc_bits_set & 0x080000)
		{
			shift_pipeline = 2;
		}
		else if (shift_pipeline != 0)
		{
			if (--shift_pipeline == 0)
			{
				shift_phase2(waveform);
			}
			else
			{
				test_or_reset = false;
				shift_latch = shift_register;
			}
		}
	}
}

uint16_t waveform_generator::output()
{
	if (waveform != 0)
	{
		const uint32_t prev_acc = prev_v ? prev_v->accumulator : 0;
		const uint32_t ix = (accumulator ^ (~prev_acc & ring_msb_mask)) >> 12;

		waveform_output = static_cast<uint16_t>((wave ? wave[ix] : 0) & (no_pulse | pulse_output) &
		                                        no_noise_or_noise_output);

		if (pulldown)
		{
			waveform_output = static_cast<uint16_t>(pulldown[waveform_output]);
		}

		if ((waveform & 3) && !is6581)
		{
			osc3 = static_cast<uint16_t>(tri_saw_pipeline & (no_pulse | pulse_output) & no_noise_or_noise_output);
			if (pulldown)
			{
				osc3 = static_cast<uint16_t>(pulldown[osc3]);
			}
			tri_saw_pipeline = static_cast<uint16_t>(wave ? wave[ix] : 0);
		}
		else
		{
			osc3 = waveform_output;
		}

		if (is6581 && (waveform & 0x2) && (waveform_output & 0x800) == 0)
		{
			msb_rising = false;
			accumulator &= 0x7FFFFF;
		}
		write_shift_register();
	}
	else
	{
		if (floating_output_ttl != 0 && --floating_output_ttl == 0) wave_bitfade();
	}

	pulse_output = ((accumulator >> 12) >= pw) ? 0xFFF : 0x000;
	return waveform_output;
}

void waveform_generator::synchronize()
{
	if (msb_rising && next_v && next_v->sync) next_v->accumulator = 0;
}

void waveform_generator::shift_phase2(int waveform_new)
{
	const uint32_t bit0 = ((shift_latch >> 22) ^ (shift_latch >> 17)) & 1;
	shift_register = ((shift_latch << 1) | (test_or_reset ? 0 : bit0)) & 0x7FFFFF;
	if (waveform_new >= 8) set_noise_output();
}

void waveform_generator::set_noise_output()
{
	// clang-format off
	noise_output = static_cast<uint16_t>(
		((shift_register & (1u << 2)) << 9) |
		((shift_register & (1u << 4)) << 6) |
		((shift_register & (1u << 8)) << 1) |
		((shift_register & (1u << 11)) >> 3) |
		((shift_register & (1u << 13)) >> 6) |
		((shift_register & (1u << 17)) >> 12) |
		((shift_register & (1u << 20)) >> 16) |
		((shift_register & (1u << 22)) >> 22));
	// clang-format on
	set_no_noise_or_noise_output();
}

void waveform_generator::write_shift_register()
{
	if ((waveform & 8) != 0 && (waveform_output & 0x800) == 0)
	{
		if (!(waveform_output & (1 << 11))) shift_register &= ~(1u << 2);
		if (!(waveform_output & (1 << 10))) shift_register &= ~(1u << 4);
		if (!(waveform_output & (1 << 9))) shift_register &= ~(1u << 8);
		if (!(waveform_output & (1 << 8))) shift_register &= ~(1u << 11);
		if (!(waveform_output & (1 << 7))) shift_register &= ~(1u << 13);
		if (!(waveform_output & (1 << 5))) shift_register &= ~(1u << 17);
		if (!(waveform_output & (1 << 4))) shift_register &= ~(1u << 20);
		if (!(waveform_output & 1)) shift_register &= ~(1u << 22);
	}
}

void waveform_generator::write_control(uint8_t control)
{
	const int waveform_new = (control >> 4) & 0x0F;
	waveform = waveform_new;

	if (model_wave) wave = model_wave[waveform_new & 3];
	pulldown = select_pulldown(model_pulldown, waveform_new);

	ring_msb_mask = (control & 0x04) ? 0x800000 : 0;
	sync = (control & 0x02) != 0;
	const bool test_old = test;
	test = (control & 0x08) != 0;

	if (waveform_new >= 8)
	{
		no_noise = 0;
		set_noise_output();
	}
	else
	{
		no_noise = 0xFFF;
		set_no_noise_or_noise_output();
	}

	no_pulse = (waveform_new >= 4 && waveform_new < 8) ? 0 : 0xFFF;

	if (!test_old && test)
	{
		accumulator = 0;
		shift_register_reset = is6581 ? 50000 : 986000;
		shift_pipeline = 0;
		pulse_output = 0xFFF;
	}
	else if (test_old && !test)
	{
		shift_latch = shift_register;
		shift_pipeline = 1;
		floating_output_ttl = is6581 ? 54000 : 800000;
	}
}

// ===========================================================================
// Envelope generator
// ===========================================================================

namespace
{
	// clang-format off
	const uint16_t ADSR_TABLE[16] = {
		0x007F, 0x3000, 0x1E00, 0x0660, 0x0182, 0x5573, 0x000E, 0x3805,
		0x2424, 0x2220, 0x090C, 0x0ECD, 0x010E, 0x23F7, 0x5237, 0x64A8
	};
	// clang-format on
}

void envelope_generator::reset()
{
	envelope_pipeline = 0;
	state_pipeline = 0;
	attack = decay = sustain = release = 0;
	gate = false;
	reset_lfsr = true;
	exponential_counter = 0;
	exponential_counter_period = 1;
	new_exponential_counter_period = 0;
	state = RELEASE;
	counter_enabled = false;
	rate = ADSR_TABLE[release];
	envelope_counter = 0;
	lfsr = 0x7FFF;
	exponential_pipeline = 0;
	env3 = 0;
	next_state = RELEASE;
}

void envelope_generator::clock()
{
	env3 = envelope_counter;

	if (new_exponential_counter_period > 0)
	{
		exponential_counter_period = new_exponential_counter_period;
		new_exponential_counter_period = 0;
	}

	if (state_pipeline) state_change();

	if (envelope_pipeline != 0 && (--envelope_pipeline == 0))
	{
		if (counter_enabled)
		{
			if (state == ATTACK)
			{
				if (++envelope_counter == 0xFF)
				{
					next_state = DECAY_SUSTAIN;
					state_pipeline = 3;
				}
			}
			else if (state == DECAY_SUSTAIN || state == RELEASE)
			{
				if (--envelope_counter == 0x00) counter_enabled = false;
			}
			set_exponential_counter();
		}
	}
	else if (exponential_pipeline != 0 && (--exponential_pipeline == 0))
	{
		exponential_counter = 0;
		if ((state == DECAY_SUSTAIN && envelope_counter != sustain) || state == RELEASE) envelope_pipeline = 1;
	}
	else if (reset_lfsr)
	{
		lfsr = 0x7FFF;
		reset_lfsr = false;
		if (state == ATTACK)
		{
			exponential_counter = 0;
			envelope_pipeline = 2;
		}
		else if (counter_enabled && (++exponential_counter == exponential_counter_period))
		{
			exponential_pipeline = exponential_counter_period != 1 ? 2 : 1;
		}
	}

	if (lfsr != rate)
	{
		const uint16_t feedback = ((lfsr << 14) ^ (lfsr << 13)) & 0x4000;
		lfsr = static_cast<uint16_t>(((lfsr >> 1) | feedback) & 0x7FFF);
	}
	else
	{
		reset_lfsr = true;
	}
}

void envelope_generator::state_change()
{
	state_pipeline--;
	switch (next_state)
	{
	case ATTACK:
		if (state_pipeline == 1) rate = ADSR_TABLE[decay];
		else if (state_pipeline == 0)
		{
			state = ATTACK;
			rate = ADSR_TABLE[attack];
			counter_enabled = true;
		}
		break;
	case DECAY_SUSTAIN:
		if (state_pipeline == 0)
		{
			state = DECAY_SUSTAIN;
			rate = ADSR_TABLE[decay];
		}
		break;
	case RELEASE:
		if ((state == ATTACK && state_pipeline == 0) || (state == DECAY_SUSTAIN && state_pipeline == 1))
		{
			state = RELEASE;
			rate = ADSR_TABLE[release];
		}
		break;
	default: break;
	}
}

void envelope_generator::set_exponential_counter()
{
	switch (envelope_counter)
	{
	case 0xFF:
	case 0x00: new_exponential_counter_period = 1; break;
	case 0x5D: new_exponential_counter_period = 2; break;
	case 0x36: new_exponential_counter_period = 4; break;
	case 0x1A: new_exponential_counter_period = 8; break;
	case 0x0E: new_exponential_counter_period = 16; break;
	case 0x06: new_exponential_counter_period = 30; break;
	default: break;
	}
}

void envelope_generator::write_control(uint8_t control)
{
	const bool gate_next = (control & 0x01) != 0;
	if (gate_next == gate) return;
	gate = gate_next;

	if (gate_next)
	{
		next_state = ATTACK;
		state_pipeline = 2;
		if (reset_lfsr || exponential_pipeline == 2)
			envelope_pipeline = (exponential_counter_period == 1 || exponential_pipeline == 2) ? 2 : 4;
		else if (exponential_pipeline == 1) state_pipeline = 3;
	}
	else
	{
		next_state = RELEASE;
		state_pipeline = envelope_pipeline > 0 ? 3 : 2;
	}
}

void envelope_generator::write_attack_decay(uint8_t v)
{
	attack = (v >> 4) & 0x0F;
	decay = v & 0x0F;
	if (state == ATTACK) rate = ADSR_TABLE[attack];
	else if (state == DECAY_SUSTAIN) rate = ADSR_TABLE[decay];
}

void envelope_generator::write_sustain_release(uint8_t v)
{
	sustain = static_cast<uint8_t>((v & 0xF0) | ((v >> 4) & 0x0F));
	release = v & 0x0F;
	if (state == RELEASE) rate = ADSR_TABLE[release];
}

// ===========================================================================
// Mixer and filter
// ===========================================================================

namespace
{
	constexpr double pi = 3.14159265358979323846;
}

double sid_filter::cutoff_hz(const bool is_6581, const int fc)
{
	if (!is_6581) return 30.0 + 6.1 * fc;

	// A logistic in log frequency, steeper below its midpoint than above it,
	// follows the measured curve's flat bottom, steep middle and slow top.
	constexpr double mid = 810.0;
	const double spread = fc < mid ? 190.0 : 260.0;
	const double rise = 1.0 / (1.0 + std::exp((mid - fc) / spread));
	return 220.0 * std::exp(4.404 * rise);
}

double sid_filter::resonance_q(const bool is_6581, const int res)
{
	const double top = is_6581 ? 1.707 : 2.2;
	return 0.707 + (top - 0.707) * (res & 0x0F) / 15.0;
}

sid_filter::sid_filter(const bool is_6581)
{
	set_model(is_6581);
	reset();
}

void sid_filter::set_model(const bool is_6581)
{
	is6581 = is_6581;
	// The 6581's waveform DAC is zero at about $380 and each voice adds a DC
	// level, less a smaller one in the mixer, all scaled by the volume register.
	// The 8580 is centred and has neither, so its $D418 digis are near silent.
	wave_zero = is_6581 ? 896.0f : 2048.0f;
	voice_dc = is_6581 ? 1.0f : 0.0f;
	mixer_dc = is_6581 ? -0.111f : 0.0f;
	q_inv = 1.0 / resonance_q(is6581, res);
	update_w0();
}

void sid_filter::set_clock_frequency(const double hz)
{
	clock_hz = hz > 0 ? hz : 985248;
	update_w0();
}

void sid_filter::update_w0()
{
	w0 = 2.0 * pi * cutoff_hz(is6581, fc) / clock_hz;
}

void sid_filter::reset()
{
	fc = 0;
	update_w0();
	write_mode_vol(0);
	write_res_filt(0);
	vhp = vbp = vlp = 0;
}

void sid_filter::write_res_filt(const uint8_t v)
{
	filt = v;
	res = (v >> 4) & 0x0F;
	q_inv = 1.0 / resonance_q(is6581, res);
	if (enabled)
	{
		filt1 = (filt & 0x01) != 0;
		filt2 = (filt & 0x02) != 0;
		filt3 = (filt & 0x04) != 0;
		filtE = (filt & 0x08) != 0;
	}
}

void sid_filter::write_mode_vol(const uint8_t v)
{
	vol = v & 0x0F;
	lp = (v & 0x10) != 0;
	bp = (v & 0x20) != 0;
	hp = (v & 0x40) != 0;
	voice3off = (v & 0x80) != 0;
}

void sid_filter::enable(const bool e)
{
	enabled = e;
	if (e) write_res_filt(filt);
	else filt1 = filt2 = filt3 = filtE = false;
}

float sid_filter::voice_output(voice& v, const bool muted) const
{
	// Clocked whatever the mute: the waveform generator's state (the combined
	// waveforms' writeback into the noise register) depends on output().
	const int wav = v.wave.output();
	if (muted) return voice_dc;
	const int env = v.envelope.output();
	return (static_cast<float>(wav) - wave_zero) * static_cast<float>(env) * (1.0f / (2048.0f * 255.0f)) + voice_dc;
}

float sid_filter::clock(voice& v1, voice& v2, voice& v3)
{
	const float a = voice_output(v1, (mute_mask & 0x01) != 0);
	const float b = voice_output(v2, (mute_mask & 0x02) != 0);
	const float c = voice_output(v3, (mute_mask & 0x04) != 0);

	// 3OFF takes voice 3 off the direct path only: routed through the filter it
	// is still heard.
	double vi = 0, vnf = 0;
	// clang-format off
	if (filt1) vi += a; else vnf += a;
	if (filt2) vi += b; else vnf += b;
	if (filt3) vi += c; else if (!voice3off) vnf += c;
	if (filtE) vi += ve; else vnf += ve;
	// clang-format on

	// The two integrators, whose outputs are inverted as the chip's are: the
	// low pass settles at -vi. Q only shapes the peak; the pass band stays at
	// unity gain whatever the resonance.
	vbp -= w0 * vhp;
	vlp -= w0 * vbp;
	vhp = vbp * q_inv - vlp - vi;

	double vf = 0;
	if (lp) vf += vlp;
	if (bp) vf += vbp;
	if (hp) vf += vhp;

	return static_cast<float>((vnf + vf + mixer_dc) * vol * (1.0 / 15.0));
}

void external_filter::set_clock_frequency(const double frequency)
{
	w_lp = 2.0 * pi * 16000.0 / frequency;
	w_hp = 2.0 * pi * 16.0 / frequency;
}

// ===========================================================================
// Resampler
// ===========================================================================

namespace
{
	double bessel_i0(const double x)
	{
		const double quarter_square = x * x / 4.0;
		double sum = 1.0;
		double term = 1.0;
		for (int k = 1; k < 64; ++k)
		{
			term *= quarter_square / (static_cast<double>(k) * k);
			sum += term;
			if (term < sum * 1e-12) break;
		}
		return sum;
	}

	// A Kaiser-windowed sinc low-pass tap: cutoff in cycles per sample, t in
	// samples from the centre, half the window's half-width.
	double windowed_sinc(const double cutoff, const double t, const double half, const double beta)
	{
		if (std::abs(t) >= half) return 0.0;
		const double x = 2.0 * cutoff * t;
		const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
		const double r = t / half;
		return 2.0 * cutoff * sinc * bessel_i0(beta * std::sqrt(1.0 - r * r)) / bessel_i0(beta);
	}
}

void sid_resampler::configure(const double clock_hz, const double output_hz)
{
	// The first stage is flat to 20 kHz and 80 dB down from the lowest frequency
	// that taking every eighth cycle folds back below 24 kHz.
	const double a_cutoff = 59200.0 / clock_hz;
	double a_sum = 0;
	for (int k = 0; k < a_taps; ++k)
	{
		const double tap = windowed_sinc(a_cutoff, k - (a_taps - 1) / 2.0, a_taps / 2.0, 7.86);
		a_coeff[k] = static_cast<float>(tap);
		a_sum += tap;
	}
	for (auto& c : a_coeff) c = static_cast<float>(c / a_sum);

	// The second cuts at the output's Nyquist frequency, 70 dB down 2 kHz past
	// it, so anything that still folds back lands above 20 kHz. Each phase's
	// row is normalised, so no phase adds a DC ripple of its own.
	const double mid_hz = clock_hz / decimation;
	b_step = mid_hz / output_hz;
	const double b_cutoff = std::min(0.5 * output_hz, 0.45 * mid_hz) / mid_hz;
	b_table.assign(static_cast<size_t>(b_phases + 1) * b_taps, 0.0f);
	for (int p = 0; p <= b_phases; ++p)
	{
		float* row = b_table.data() + static_cast<size_t>(p) * b_taps;
		const double offset = static_cast<double>(p) / b_phases - b_taps / 2.0;
		double row_sum = 0;
		for (int j = 0; j < b_taps; ++j)
		{
			const double tap = windowed_sinc(b_cutoff, j + offset, b_taps / 2.0, 6.76);
			row[j] = static_cast<float>(tap);
			row_sum += tap;
		}
		for (int j = 0; j < b_taps; ++j) row[j] = static_cast<float>(row[j] / row_sum);
	}

	reset();
}

void sid_resampler::reset()
{
	std::memset(a_ring, 0, sizeof(a_ring));
	std::memset(b_ring, 0, sizeof(b_ring));
	a_pos = 0;
	a_count = 0;
	b_pos = 0;
	b_acc = 0;
}

bool sid_resampler::decimated(float& out)
{
	// a_ring and b_ring hold each sample twice, so the window from the oldest
	// sample on is always contiguous. Four running sums rather than one let the
	// additions overlap instead of waiting on each other.
	const float* a = a_ring + a_pos;
	float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
	for (int k = 0; k < a_taps; k += 4)
	{
		s0 += a_coeff[k] * a[k];
		s1 += a_coeff[k + 1] * a[k + 1];
		s2 += a_coeff[k + 2] * a[k + 2];
		s3 += a_coeff[k + 3] * a[k + 3];
	}
	const float y = (s0 + s1) + (s2 + s3);

	b_ring[b_pos] = y;
	b_ring[b_pos + b_taps] = y;
	if (++b_pos == b_taps) b_pos = 0;

	b_acc += 1.0;
	if (b_acc < b_step) return false;
	b_acc -= b_step;

	// The output is due b_acc decimated samples before the newest one: blend the
	// two tabulated phases either side of it.
	const double phase = b_acc * b_phases;
	int p = static_cast<int>(phase);
	if (p >= b_phases) p = b_phases - 1;
	const float frac = static_cast<float>(phase - p);
	const float* row0 = b_table.data() + static_cast<size_t>(p) * b_taps;
	const float* row1 = row0 + b_taps;
	const float* x = b_ring + b_pos;
	float p0 = 0, p1 = 0, q0 = 0, q1 = 0;
	for (int j = 0; j < b_taps; j += 2)
	{
		p0 += row0[j] * x[j];
		p1 += row0[j + 1] * x[j + 1];
		q0 += row1[j] * x[j];
		q1 += row1[j + 1] * x[j + 1];
	}
	const float y0 = p0 + p1;
	const float y1 = q0 + q1;
	out = y0 + (y1 - y0) * frac;
	return true;
}

// ===========================================================================
// SID chip
// ===========================================================================

sid_chip::sid_chip() : filter(true)
{
	sid_init_tables();

	voices[0].wave.set_other(&voices[2].wave, &voices[1].wave);
	voices[1].wave.set_other(&voices[0].wave, &voices[2].wave);
	voices[2].wave.set_other(&voices[1].wave, &voices[0].wave);

	set_chip_model(sid_model::mos6581);
	set_sampling_parameters(985248, 44100);
	reset();
}

void sid_chip::set_chip_model(const sid_model m)
{
	const bool is6581 = m == sid_model::mos6581;
	model_ttl = is6581 ? 0x01D00 : 0xA2000;
	model = m;
	filter.set_model(is6581);

	const int model_idx = is6581 ? 0 : 1;
	for (auto& v : voices)
	{
		v.wave.set_model(is6581);
		v.wave.set_waveform_models(g_wftable);
		v.wave.set_pulldown_models(g_pulldown[model_idx]);
	}
}

void sid_chip::set_sampling_parameters(const double clk, double samp)
{
	if (clk <= 0 || samp <= 0) return;
	// One output sample per decimated sample at most, which the chip's clock
	// leaves room for up to 96 kHz.
	samp = std::clamp(samp, 8000.0, 96000.0);
	if (clk == clock_frequency && samp == sampling_frequency) return;

	clock_frequency = clk;
	sampling_frequency = samp;
	filter.set_clock_frequency(clk);
	ext_filter.set_clock_frequency(clk);
	resampler.configure(clk, samp);
}

void sid_chip::reset()
{
	for (auto& v : voices) v.reset();
	filter.reset();
	ext_filter.reset();
	resampler.reset();
	bus_value = 0;
	bus_value_ttl = 0;
	write_queue.clear();
	voice_sync(false);
}

void sid_chip::set_voice_muted(const int v, const bool muted)
{
	if (v < 0 || v > 2) return;
	const uint8_t bit = static_cast<uint8_t>(1 << v);
	const uint8_t mask = filter.get_mute_mask();
	filter.set_mute_mask(muted ? (mask | bit) : (mask & ~bit));
}

bool sid_chip::voice_muted(const int v) const
{
	return v >= 0 && v <= 2 && (filter.get_mute_mask() & (1 << v)) != 0;
}

uint8_t sid_chip::read(uint16_t offset)
{
	switch (offset)
	{
	case 0x19:
	case 0x1A:
		bus_value = 0xFF;
		bus_value_ttl = model_ttl;
		break;
	case 0x1B:
		bus_value = voices[2].wave.read_osc();
		bus_value_ttl = model_ttl;
		break;
	case 0x1C:
		bus_value = voices[2].envelope.read_env();
		bus_value_ttl = model_ttl;
		break;
	default: bus_value_ttl /= 2; break;
	}
	return bus_value;
}

void sid_chip::write(uint16_t offset, uint8_t value, int64_t cycle)
{
	// Bound the queue in case a host enables audio but never drains it.
	constexpr size_t max_queued = 1 << 16;
	if (write_queue.size() >= max_queued)
		write_queue.erase(write_queue.begin(), write_queue.begin() + static_cast<std::ptrdiff_t>(max_queued / 2));

	write_queue.push_back({static_cast<uint8_t>(offset & 0x1F), value, cycle});
}

void sid_chip::apply_register_write(uint8_t offset, uint8_t value)
{
	bus_value = value;
	bus_value_ttl = model_ttl;

	switch (offset)
	{
	case 0x00: voices[0].wave.write_freq_lo(value); break;
	case 0x01: voices[0].wave.write_freq_hi(value); break;
	case 0x02: voices[0].wave.write_pw_lo(value); break;
	case 0x03: voices[0].wave.write_pw_hi(value); break;
	case 0x04: voices[0].write_control(value); break;
	case 0x05: voices[0].envelope.write_attack_decay(value); break;
	case 0x06: voices[0].envelope.write_sustain_release(value); break;
	case 0x07: voices[1].wave.write_freq_lo(value); break;
	case 0x08: voices[1].wave.write_freq_hi(value); break;
	case 0x09: voices[1].wave.write_pw_lo(value); break;
	case 0x0A: voices[1].wave.write_pw_hi(value); break;
	case 0x0B: voices[1].write_control(value); break;
	case 0x0C: voices[1].envelope.write_attack_decay(value); break;
	case 0x0D: voices[1].envelope.write_sustain_release(value); break;
	case 0x0E: voices[2].wave.write_freq_lo(value); break;
	case 0x0F: voices[2].wave.write_freq_hi(value); break;
	case 0x10: voices[2].wave.write_pw_lo(value); break;
	case 0x11: voices[2].wave.write_pw_hi(value); break;
	case 0x12: voices[2].write_control(value); break;
	case 0x13: voices[2].envelope.write_attack_decay(value); break;
	case 0x14: voices[2].envelope.write_sustain_release(value); break;
	case 0x15: filter.write_fc_lo(value); break;
	case 0x16: filter.write_fc_hi(value); break;
	case 0x17: filter.write_res_filt(value); break;
	case 0x18: filter.write_mode_vol(value); break;
	default: break;
	}
	voice_sync(false);
}

void sid_chip::age_bus_value(int n)
{
	if (bus_value_ttl != 0)
	{
		bus_value_ttl -= n;
		if (bus_value_ttl <= 0)
		{
			bus_value = 0;
			bus_value_ttl = 0;
		}
	}
}

void sid_chip::voice_sync(bool sync)
{
	if (sync)
		for (auto& v : voices) v.wave.synchronize();

	next_voice_sync = 0x7FFFFFFF;
	for (auto& v : voices)
	{
		const uint16_t freq = v.wave.read_freq();
		if (v.wave.read_test() || freq == 0 || !v.wave.read_following_voice_sync()) continue;
		const uint32_t acc = v.wave.read_accumulator();
		const int64_t this_sync = ((0x7FFFFF - acc) & 0xFFFFFF) / freq + 1;
		if (this_sync < next_voice_sync) next_voice_sync = this_sync;
	}
}

int16_t sid_chip::output_sample(const float value) const
{
	// A voice at full envelope swings about 7000 either way. Past three quarters
	// of full scale the output bends smoothly towards it instead of clipping, so
	// a resonant peak or the 6581's volume step never turns into a hard edge.
	constexpr float gain = 7000.0f;
	constexpr float knee = 24576.0f;
	constexpr float room = 32767.0f - knee;
	float x = value * gain;
	const float magnitude = std::abs(x);
	if (magnitude > knee) x = std::copysign(knee + room * std::tanh((magnitude - knee) / room), x);
	return static_cast<int16_t>(std::lround(x));
}

int sid_chip::clock(int cycles, int16_t* buf, int64_t start_cycle, int max_samples)
{
	age_bus_value(cycles);
	int s = 0;
	const int64_t end_cycle = start_cycle + cycles;
	int64_t current_cycle = start_cycle;
	size_t queue_index = 0;

	while (current_cycle < end_cycle)
	{
		while (queue_index < write_queue.size() && write_queue[queue_index].cycle <= current_cycle)
		{
			const queued_write& cmd = write_queue[queue_index++];
			apply_register_write(cmd.offset, cmd.value);
		}

		int64_t next_event = end_cycle;
		if (queue_index < write_queue.size()) next_event = std::min(next_event, write_queue[queue_index].cycle);

		const int64_t to_run = std::min(next_event - current_cycle, next_voice_sync);
		if (to_run <= 0)
		{
			current_cycle++;
			continue;
		}

		for (int64_t i = 0; i < to_run; i++)
		{
			voices[0].wave.clock();
			voices[1].wave.clock();
			voices[2].wave.clock();
			voices[0].envelope.clock();
			voices[1].envelope.clock();
			voices[2].envelope.clock();

			const float mixed = filter.clock(voices[0], voices[1], voices[2]);
			const auto heard = static_cast<float>(ext_filter.clock(mixed));

			float sample;
			if (resampler.input(heard, sample) && s < max_samples) buf[s++] = output_sample(sample);
		}

		current_cycle += to_run;
		next_voice_sync -= to_run;
		if (next_voice_sync <= 0) voice_sync(true);
	}

	write_queue.erase(write_queue.begin(), write_queue.begin() + static_cast<std::ptrdiff_t>(queue_index));

	return s;
}
