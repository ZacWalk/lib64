// lib64 by Zac Walker
//
// 6502 CPU core: instruction execution, addressing modes, arithmetic and
// flag handling, the stack, and IRQ/NMI interrupt servicing.

#include <stdint.h>

#include "cpu.h"
#include "machine.h"
#include "opcodes.h"

#define BASE_STACK 0x100

// Everything below is internal to the CPU core; only cpu_exec, cpu_irq and
// nmi6502 have external linkage so lib64 can be linked into a host app.
namespace
{
	// Entry point of the KERNAL's own LOAD routine, i.e. the default contents of
	// the LOAD vector at $0330 (see machine_state::kernal_load).
	constexpr uint16_t KERNAL_LOAD_TRAP = 0xF4A5;
	// Where the screen editor loops waiting for a key: an autostart types there.
	constexpr uint16_t KERNAL_EDITOR_WAIT = 0xE5CD;
	// general functions used by various other functions
	void push16(machine_state* s, const uint16_t pushval)
	{
		s->ram_write(BASE_STACK + s->cpu.sp, (pushval >> 8) & 0xFF);
		s->ram_write(BASE_STACK + ((s->cpu.sp - 1) & 0xFF), pushval & 0xFF);
		s->cpu.sp -= 2;
	}

	void push8(machine_state* s, const uint8_t pushval)
	{
		s->ram_write(BASE_STACK + s->cpu.sp--, pushval);
	}

	uint16_t pull16(machine_state* s)
	{
		const auto temp16 = static_cast<uint16_t>(s->ram_read(BASE_STACK + ((s->cpu.sp + 1) & 0xFF)) |
		                                          (s->ram_read(BASE_STACK + ((s->cpu.sp + 2) & 0xFF)) << 8));
		s->cpu.sp += 2;
		return temp16;
	}

	uint8_t pull8(machine_state* s)
	{
		return (s->ram_read(BASE_STACK + ++s->cpu.sp));
	}

	// addressing mode functions, calculates effective addresses
	void imp(machine_state*)
	{
		// implied
	}

	uint16_t imm(machine_state* s)
	{
		// immediate
		return s->cpu.pc++;
	}

	uint16_t zp(machine_state* s)
	{
		// zero-page
		return s->ram_read(s->cpu.pc++);
	}

	uint16_t zpx(machine_state* s)
	{
		// zero-page,s->cpu.x
		// zero-page wraparound
		return (static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s->cpu.x)) & 0xFF;
	}

	uint16_t zpy(machine_state* s)
	{
		// zero-page,s->cpu.y
		// zero-page wraparound
		return (static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s->cpu.y)) & 0xFF;
	}

	uint16_t rel(machine_state* s)
	{
		// relative for branch ops (8-bit immediate value, sign-extended)
		auto reladdr = static_cast<uint16_t>(s->ram_read(s->cpu.pc++));
		if (reladdr & 0x80) reladdr |= 0xFF00;
		return reladdr;
	}

	uint16_t abso(machine_state* s)
	{
		// absolute
		const auto ea = static_cast<uint16_t>(s->ram_read(s->cpu.pc) | (s->ram_read(s->cpu.pc + 1) << 8));
		s->cpu.pc += 2;
		return ea;
	}

	uint16_t absx(machine_state* s)
	{
		// absolute,s->cpu.x
		const auto base = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) |
		                  (static_cast<uint16_t>(s->ram_read(s->cpu.pc + 1)) << 8);
		const auto ea = static_cast<uint16_t>(base + s->cpu.x);
		if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
		s->cpu.pc += 2;
		return ea;
	}

	uint16_t absy(machine_state* s)
	{
		// absolute,s->cpu.y
		const auto base = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) |
		                  (static_cast<uint16_t>(s->ram_read(s->cpu.pc + 1)) << 8);
		const auto ea = static_cast<uint16_t>(base + s->cpu.y);
		if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
		s->cpu.pc += 2;
		return ea;
	}

	uint16_t ind(machine_state* s)
	{
		// indirect
		const auto eahelp = static_cast<uint16_t>(s->ram_read(s->cpu.pc) | (s->ram_read(s->cpu.pc + 1) << 8));
		const auto eahelp2 = static_cast<uint16_t>(
			(eahelp & 0xFF00) | ((eahelp + 1) & 0x00FF)); // replicate 6502 page-boundary wraparound bug
		const auto ea = static_cast<uint16_t>(s->ram_read(eahelp) | (s->ram_read(eahelp2) << 8));
		s->cpu.pc += 2;
		return ea;
	}

	uint16_t indx(machine_state* s)
	{
		// (indirect,s->cpu.x)
		const auto eahelp = static_cast<uint16_t>(
			(static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s->cpu.x)) & 0xFF);
		// zero-page wraparound for table pointer
		const auto ea = static_cast<uint16_t>(s->ram_read(eahelp & 0x00FF) | (s->ram_read((eahelp + 1) & 0x00FF) << 8));
		return ea;
	}

	uint16_t indy(machine_state* s)
	{
		// (indirect),s->cpu.y

		const auto ea1 = static_cast<uint16_t>(s->ram_read(s->cpu.pc++));
		const auto ea2 = static_cast<uint16_t>((ea1 & 0xFF00) | ((ea1 + 1) & 0x00FF)); // zero-page wraparound
		const auto base = static_cast<uint16_t>(s->ram_read(ea1)) | (static_cast<uint16_t>(s->ram_read(ea2)) << 8);
		const auto ea = static_cast<uint16_t>(base + s->cpu.y);
		if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
		return ea;
	}

	static uint16_t get_ea(machine_state* s, const uint16_t ea)
	{
		return s->ram_read(ea);
	}

	static uint16_t get_a(const machine_state* s)
	{
		return s->cpu.a;
	}

	void put_ea(machine_state* s, const uint16_t saveval, const uint16_t ea)
	{
		s->ram_write(ea, (saveval & 0x00FF));
	}

	void put_a(machine_state* s, const uint16_t saveval)
	{
		s->cpu.a = static_cast<uint8_t>(saveval & 0x00FF);
	}

	// instruction handler functions
	uint16_t adc(machine_state* s, const uint16_t value)
	{
		const auto operand = static_cast<uint16_t>(value & 0x00FF);
		const auto carry_in = static_cast<uint16_t>(s->cpu.status & FLAG_CARRY);
		const auto result = static_cast<uint16_t>(s->cpu.a + operand + carry_in);

		// N, Z, V, C come from the binary sum on the NMOS 6502/6510.
		s->cpu.calc_carry(result);
		s->cpu.calc_zero(result);
		s->cpu.calc_overflow(result, s->cpu.a, operand);
		s->cpu.calc_sign(result);

		if (s->cpu.status & FLAG_DECIMAL)
		{
			// Proper BCD addition with carry propagation between nybbles. On the NMOS
			// 6502 Z comes from the binary sum above, but N and V are taken from the
			// intermediate result after the low-nybble fixup and before the high one.
			unsigned al = (s->cpu.a & 0x0F) + (operand & 0x0F) + carry_in;
			if (al > 0x09) al = ((al + 0x06) & 0x0F) + 0x10;
			unsigned bcd = (s->cpu.a & 0xF0) + (operand & 0xF0) + al;

			s->cpu.calc_sign(static_cast<uint16_t>(bcd));
			if ((~(s->cpu.a ^ operand) & (s->cpu.a ^ bcd) & 0x80) != 0) s->cpu.set_overflow();
			else s->cpu.clear_overflow();

			if (bcd >= 0xA0) bcd += 0x60;
			if (bcd > 0xFF) s->cpu.set_carry();
			else s->cpu.clear_carry();

			return static_cast<uint16_t>(bcd & 0xFF);
		}

		return result;
	}

	uint16_t op_and(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.a & value);

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	uint16_t asl(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(value << 1);

		s->cpu.calc_carry(result);
		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	void bcc(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_CARRY) == 0)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void bcs(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_CARRY) == FLAG_CARRY)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void beq(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_ZERO) == FLAG_ZERO)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void op_bit(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.a & value);

		s->cpu.calc_zero(result);
		s->cpu.status = (s->cpu.status & 0x3F) | static_cast<uint8_t>(value & 0xC0);
	}

	void bmi(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_SIGN) == FLAG_SIGN)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void bne(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_ZERO) == 0)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void bpl(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_SIGN) == 0)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void brk(machine_state* s)
	{
		s->cpu.pc++;
		push16(s, s->cpu.pc);                 // push next instruction address onto stack
		push8(s, s->cpu.status | FLAG_BREAK); // push CPU s->cpu.status to stack
		s->cpu.set_interrupt();               // set interrupt flag
		s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFE)) | (static_cast<uint16_t>(s->ram_read(0xFFFF)) << 8);
	}

	void bvc(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_OVERFLOW) == 0)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void bvs(machine_state* s, const uint16_t reladdr)
	{
		if ((s->cpu.status & FLAG_OVERFLOW) == FLAG_OVERFLOW)
		{
			const auto old_pc = s->cpu.pc;
			s->cpu.pc += reladdr;
			if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			// check if jump crossed s->cpu.a page boundary
			else s->clock_ticks++;
		}
	}

	void clc(machine_state* s)
	{
		s->cpu.clear_carry();
	}

	void cld(machine_state* s)
	{
		s->cpu.clear_decimal();
	}

	void cli(machine_state* s)
	{
		s->cpu.clear_interrupt();
	}

	void clv(machine_state* s)
	{
		s->cpu.clear_overflow();
	}

	void cmp(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.a - value);

		if (s->cpu.a >= static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_carry();
		else s->cpu.clear_carry();

		if (s->cpu.a == static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_zero();
		else s->cpu.clear_zero();

		s->cpu.calc_sign(result);
	}

	void cpx(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.x - value);

		if (s->cpu.x >= static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_carry();
		else s->cpu.clear_carry();

		if (s->cpu.x == static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_zero();
		else s->cpu.clear_zero();

		s->cpu.calc_sign(result);
	}

	void cpy(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.y - value);

		if (s->cpu.y >= static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_carry();
		else s->cpu.clear_carry();
		if (s->cpu.y == static_cast<uint8_t>(value & 0x00FF)) s->cpu.set_zero();
		else s->cpu.clear_zero();
		s->cpu.calc_sign(result);
	}

	uint16_t dec(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(value - 1);

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	void dex(machine_state* s)
	{
		s->cpu.x--;

		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void dey(machine_state* s)
	{
		s->cpu.y--;

		s->cpu.calc_zero(s->cpu.y);
		s->cpu.calc_sign(s->cpu.y);
	}

	uint16_t eor(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.a ^ value);

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	uint16_t inc(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(value + 1);

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	void inx(machine_state* s)
	{
		s->cpu.x++;

		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void iny(machine_state* s)
	{
		s->cpu.y++;

		s->cpu.calc_zero(s->cpu.y);
		s->cpu.calc_sign(s->cpu.y);
	}

	void jmp(machine_state* s, const uint16_t ea)
	{
		s->cpu.pc = ea;
	}

	void jsr(machine_state* s, const uint16_t ea)
	{
		push16(s, s->cpu.pc - 1);
		s->cpu.pc = ea;
	}

	void lda(machine_state* s, const uint16_t value)
	{
		s->cpu.a = static_cast<uint8_t>(value & 0x00FF);

		s->cpu.calc_zero(s->cpu.a);
		s->cpu.calc_sign(s->cpu.a);
	}

	void ldx(machine_state* s, const uint16_t value)
	{
		s->cpu.x = static_cast<uint8_t>(value & 0x00FF);

		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void ldy(machine_state* s, const uint16_t value)
	{
		s->cpu.y = static_cast<uint8_t>(value & 0x00FF);

		s->cpu.calc_zero(s->cpu.y);
		s->cpu.calc_sign(s->cpu.y);
	}

	uint16_t lsr(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(value >> 1);

		if (value & 1) s->cpu.set_carry();
		else s->cpu.clear_carry();

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	uint16_t ora(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>(s->cpu.a | value);

		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	void pha(machine_state* s)
	{
		push8(s, s->cpu.a);
	}

	void php(machine_state* s)
	{
		push8(s, s->cpu.status | FLAG_BREAK);
	}

	void pla(machine_state* s)
	{
		s->cpu.a = pull8(s);

		s->cpu.calc_zero(s->cpu.a);
		s->cpu.calc_sign(s->cpu.a);
	}

	void plp(machine_state* s)
	{
		s->cpu.status = pull8(s) | FLAG_CONSTANT;
	}

	uint16_t rol(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>((value << 1) | (s->cpu.status & FLAG_CARRY));

		s->cpu.calc_carry(result);
		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	uint16_t ror(machine_state* s, const uint16_t value)
	{
		const auto result = static_cast<uint16_t>((value >> 1) | ((s->cpu.status & FLAG_CARRY) << 7));

		if (value & 1) s->cpu.set_carry();
		else s->cpu.clear_carry();
		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);

		return result;
	}

	void rti(machine_state* s)
	{
		// On RTI the BREAK flag must be cleared and the unused CONSTANT bit forced on.
		s->cpu.status = (pull8(s) & ~FLAG_BREAK) | FLAG_CONSTANT;
		s->cpu.pc = pull16(s);
	}

	void rts(machine_state* s)
	{
		const auto value = pull16(s);
		s->cpu.pc = value + 1;
	}

	uint16_t sbc(machine_state* s, const uint16_t value)
	{
		const auto operand = static_cast<uint16_t>(value & 0x00FF);
		const auto inv = static_cast<uint16_t>(operand ^ 0x00FF);
		const auto carry_in = static_cast<uint16_t>(s->cpu.status & FLAG_CARRY);
		const auto result = static_cast<uint16_t>(s->cpu.a + inv + carry_in);

		// N, Z, V, C are computed from the binary result on both NMOS 6502/6510.
		s->cpu.calc_carry(result);
		s->cpu.calc_zero(result);
		s->cpu.calc_overflow(result, s->cpu.a, inv);
		s->cpu.calc_sign(result);

		if (s->cpu.status & FLAG_DECIMAL)
		{
			// Standard NMOS BCD subtract with borrow.
			int al = (s->cpu.a & 0x0F) - (operand & 0x0F) - (carry_in ? 0 : 1);
			int ah = (s->cpu.a >> 4) - (operand >> 4);
			if (al < 0)
			{
				al -= 0x06;
				ah--;
			}
			if (ah < 0)
			{
				ah -= 0x06;
			}
			return static_cast<uint16_t>(((ah << 4) | (al & 0x0F)) & 0xFF);
		}

		return result;
	}

	void sec(machine_state* s)
	{
		s->cpu.set_carry();
	}

	void sed(machine_state* s)
	{
		s->cpu.set_decimal();
	}

	void sei(machine_state* s)
	{
		s->cpu.set_interrupt();
	}

	void tax(machine_state* s)
	{
		s->cpu.x = s->cpu.a;

		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void tay(machine_state* s)
	{
		s->cpu.y = s->cpu.a;

		s->cpu.calc_zero(s->cpu.y);
		s->cpu.calc_sign(s->cpu.y);
	}

	void tsx(machine_state* s)
	{
		s->cpu.x = s->cpu.sp;

		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void txa(machine_state* s)
	{
		s->cpu.a = s->cpu.x;

		s->cpu.calc_zero(s->cpu.a);
		s->cpu.calc_sign(s->cpu.a);
	}

	void txs(machine_state* s)
	{
		s->cpu.sp = s->cpu.x;
	}

	void tya(machine_state* s)
	{
		s->cpu.a = s->cpu.y;

		s->cpu.calc_zero(s->cpu.a);
		s->cpu.calc_sign(s->cpu.a);
	}
} // namespace

void nmi6502(machine_state* s)
{
	push16(s, s->cpu.pc);
	// Hardware interrupts push status with BREAK cleared and CONSTANT set.
	push8(s, (s->cpu.status | FLAG_CONSTANT) & ~FLAG_BREAK);
	s->cpu.set_interrupt();
	s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFA)) | (static_cast<uint16_t>(s->ram_read(0xFFFB)) << 8);
}

void cpu_irq(machine_state* s)
{
	push16(s, s->cpu.pc);
	// A hardware IRQ pushes only PC and status with BREAK cleared and CONSTANT set.
	push8(s, (s->cpu.status | FLAG_CONSTANT) & ~FLAG_BREAK);
	s->cpu.set_interrupt();
	// Vector through the real 6502 IRQ vector at $FFFE, which on the C64 points at
	// the kernal entry ($FF48). That entry saves A/X/Y on the stack before doing
	// JMP ($0314) to the user handler at $EA31; the handler restores A/X/Y and RTIs.
	// Jumping straight to $0314 here would skip the register save, so the handler's
	// closing PLA/PLA/PLA would unbalance the stack and RTI to a garbage address.
	s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFE)) | (static_cast<uint16_t>(s->ram_read(0xFFFF)) << 8);
}

namespace
{
	// Read instructions using abs,X / abs,Y / (ind),Y that pay the page-cross
	// penalty (one extra cycle when the effective address crosses a page).
	constexpr bool penalty_op(const uint8_t opcode)
	{
		// clang-format off
		switch (opcode)
		{
		case 0x1D: case 0x19: case 0x11: // ORA
		case 0x3D: case 0x39: case 0x31: // AND
		case 0x5D: case 0x59: case 0x51: // EOR
		case 0x7D: case 0x79: case 0x71: // ADC
		case 0xBD: case 0xB9: case 0xB1: // LDA
		case 0xBE:                       // LDX abs,Y
		case 0xBC:                       // LDY abs,X
		case 0xDD: case 0xD9: case 0xD1: // CMP
		case 0xFD: case 0xF9: case 0xF1: // SBC
		case 0xBF: case 0xB3:            // LAX abs,Y / (ind),Y
		case 0xBB:                       // LAS abs,Y
		case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: // NOP abs,X
			return true;
		default:
			return false;
		}
		// clang-format on
	}

	// clang-format off
	constexpr uint8_t ticktable[256] = {
		7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
		6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
		6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
		6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
		2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
		2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
		2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
		2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
		2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
		2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
		2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7
	};
	// clang-format on

	// A 6502 read-modify-write instruction writes the unmodified byte back before
	// the modified one. C64 code depends on that dummy write reaching I/O, which is
	// what makes INC $D019 / LSR $D019 / DEC $DC0D acknowledge an interrupt.
	void rmw(machine_state* s, const uint16_t ea, uint16_t (*op)(machine_state*, uint16_t))
	{
		const uint16_t value = get_ea(s, ea);
		put_ea(s, value, ea);
		put_ea(s, op(s, value), ea);
	}

	// ---------------------------------------------------------------------------
	// Undocumented opcodes. The stable ones are what the NMOS 6510 does every time,
	// and crunchers, loaders and games use them. The unstable ones (ANE, LXA and
	// the SHx stores) use the "magic" constant and address behaviour most C64s show.
	// ---------------------------------------------------------------------------

	// The read-modify-write combinations: the shift, rotate or step on memory, with
	// its dummy write, then the result combined with A.
	uint8_t rmw_result(machine_state* s, const uint16_t ea, uint16_t (*op)(machine_state*, uint16_t))
	{
		const uint16_t value = get_ea(s, ea);
		put_ea(s, value, ea);
		const auto result = static_cast<uint8_t>(op(s, value));
		put_ea(s, result, ea);
		return result;
	}

	// clang-format off
	void slo(machine_state* s, const uint16_t ea) { put_a(s, ora(s, rmw_result(s, ea, asl))); }
	void rla(machine_state* s, const uint16_t ea) { put_a(s, op_and(s, rmw_result(s, ea, rol))); }
	void sre(machine_state* s, const uint16_t ea) { put_a(s, eor(s, rmw_result(s, ea, lsr))); }
	void rra(machine_state* s, const uint16_t ea) { put_a(s, adc(s, rmw_result(s, ea, ror))); }
	void dcp(machine_state* s, const uint16_t ea) { cmp(s, rmw_result(s, ea, dec)); }
	void isc(machine_state* s, const uint16_t ea) { put_a(s, sbc(s, rmw_result(s, ea, inc))); }
	// clang-format on

	void lax(machine_state* s, const uint16_t value)
	{
		lda(s, value);
		s->cpu.x = s->cpu.a;
	}

	void sax(machine_state* s, const uint16_t ea)
	{
		put_ea(s, s->cpu.a & s->cpu.x, ea);
	}

	void anc(machine_state* s, const uint16_t value)
	{
		put_a(s, op_and(s, value));
		if (s->cpu.a & 0x80) s->cpu.set_carry();
		else s->cpu.clear_carry();
	}

	void alr(machine_state* s, const uint16_t value)
	{
		put_a(s, lsr(s, op_and(s, value)));
	}

	// AND then ROR, with the flags taken from the adder the ROR shares: in binary
	// mode C is bit 6 and V is bit 6 XOR bit 5; in decimal mode each nybble of the
	// AND is corrected as ADC would correct it.
	void arr(machine_state* s, const uint16_t value)
	{
		const uint8_t anded = s->cpu.a & static_cast<uint8_t>(value);
		auto result = static_cast<uint8_t>((anded >> 1) | ((s->cpu.status & FLAG_CARRY) << 7));

		if (s->cpu.status & FLAG_DECIMAL)
		{
			if (s->cpu.status & FLAG_CARRY) s->cpu.set_sign();
			else s->cpu.clear_sign();
			s->cpu.calc_zero(result);
			if ((anded ^ result) & 0x40) s->cpu.set_overflow();
			else s->cpu.clear_overflow();
			if ((anded & 0x0F) + (anded & 0x01) > 0x05)
				result = static_cast<uint8_t>((result & 0xF0) | ((result + 0x06) & 0x0F));
			if ((anded & 0xF0) + (anded & 0x10) > 0x50)
			{
				result = static_cast<uint8_t>((result & 0x0F) | ((result + 0x60) & 0xF0));
				s->cpu.set_carry();
			}
			else s->cpu.clear_carry();
		}
		else
		{
			s->cpu.calc_zero(result);
			s->cpu.calc_sign(result);
			if (result & 0x40) s->cpu.set_carry();
			else s->cpu.clear_carry();
			if (((result >> 6) ^ (result >> 5)) & 0x01) s->cpu.set_overflow();
			else s->cpu.clear_overflow();
		}
		s->cpu.a = result;
	}

	// X = (A AND X) - operand, setting N, Z and C as CMP would and leaving V and
	// decimal mode alone.
	void sbx(machine_state* s, const uint16_t value)
	{
		const uint8_t ax = s->cpu.a & s->cpu.x;
		const auto operand = static_cast<uint8_t>(value);
		if (ax >= operand) s->cpu.set_carry();
		else s->cpu.clear_carry();
		s->cpu.x = static_cast<uint8_t>(ax - operand);
		s->cpu.calc_zero(s->cpu.x);
		s->cpu.calc_sign(s->cpu.x);
	}

	void las(machine_state* s, const uint16_t value)
	{
		const uint8_t result = static_cast<uint8_t>(value) & s->cpu.sp;
		s->cpu.a = s->cpu.x = s->cpu.sp = result;
		s->cpu.calc_zero(result);
		s->cpu.calc_sign(result);
	}

	constexpr uint8_t UNSTABLE_MAGIC = 0xEE;

	void ane(machine_state* s, const uint16_t value)
	{
		lda(s, (s->cpu.a | UNSTABLE_MAGIC) & s->cpu.x & static_cast<uint8_t>(value));
	}

	void lxa(machine_state* s, const uint16_t value)
	{
		lax(s, (s->cpu.a | UNSTABLE_MAGIC) & static_cast<uint8_t>(value));
	}

	// SHA, SHX, SHY and TAS store a register ANDed with the high byte of the base
	// address plus one; when the index crosses a page, that value also becomes the
	// high byte of the address written.
	void sh_store(machine_state* s, const uint16_t base, const uint8_t index, const uint8_t reg)
	{
		const auto value = static_cast<uint8_t>(reg & ((base >> 8) + 1));
		auto ea = static_cast<uint16_t>(base + index);
		if ((ea ^ base) & 0xFF00) ea = static_cast<uint16_t>((ea & 0x00FF) | (value << 8));
		put_ea(s, value, ea);
	}

	uint16_t abs_base(machine_state* s)
	{
		const auto base = static_cast<uint16_t>(s->ram_read(s->cpu.pc) | (s->ram_read(s->cpu.pc + 1) << 8));
		s->cpu.pc += 2;
		return base;
	}

	uint16_t indy_base(machine_state* s)
	{
		const uint8_t zp_addr = s->ram_read(s->cpu.pc++);
		return static_cast<uint16_t>(s->ram_read(zp_addr) | (s->ram_read(static_cast<uint8_t>(zp_addr + 1)) << 8));
	}

	// Service a pending interrupt at an instruction boundary, mirroring the 6510's
	// behaviour: NMI is edge-triggered and always taken; IRQ is level-triggered and
	// taken only while the interrupt-disable flag is clear. Either takes 7 cycles.
	static bool cpu_service_interrupts(machine_state* s)
	{
		if (s->nmi_pending)
		{
			s->nmi_pending = false;
			nmi6502(s);
			s->clock_ticks += 7;
			return true;
		}

		if (s->irq_pending && (s->cpu.status & FLAG_INTERRUPT) == 0)
		{
			s->ext_irq = false; // a host-raised IRQ is a pulse, not a held line
			s->update_irq_line();
			cpu_irq(s);
			s->clock_ticks += 7;
			return true;
		}

		return false;
	}
} // namespace

void cpu_exec(machine_state* s, const int32_t tick_count)
{
	uint16_t ea;

	int64_t clock_limit = s->clock_ticks + tick_count;

	while (clock_limit > s->clock_ticks)
	{
		const int64_t cycles_before = s->clock_ticks;

		if (cpu_service_interrupts(s))
		{
			s->step_hardware(static_cast<int32_t>(s->clock_ticks - cycles_before));
			continue;
		}

		// $F4A5 is where the LOAD vector at $0330 points by default. Servicing it
		// from the mounted disk image is what lets a program load further files;
		// a program that revectors $0330 to its own loader is not intercepted.
		if (s->cpu.pc == KERNAL_LOAD_TRAP && !s->disk.empty() && !s->raw_ram && s->read_map[0xF4] == 2 &&
		    s->kernal_load())
			continue;

		if (s->cpu.pc == KERNAL_EDITOR_WAIT && (!s->typing.empty() || !s->pending_prg.empty()) && !s->raw_ram &&
		    s->read_map[0xE5] == 2)
			s->editor_waiting();

		const auto opcode = s->ram_read(s->cpu.pc++);
		s->cpu.status |= FLAG_CONSTANT;
		s->penalty_addr = 0;

		switch (opcode)
		{
		case 0x0:
			imp(s);
			brk(s);
			break;
		case 0x1:
			ea = indx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x5:
			ea = zp(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x6:
			ea = zp(s);
			rmw(s, ea, asl);
			break;
		case 0x8:
			imp(s);
			php(s);
			break;
		case 0x9:
			ea = imm(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0xA: put_a(s, asl(s, get_a(s))); break;
		case 0xD:
			ea = abso(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0xE:
			ea = abso(s);
			rmw(s, ea, asl);
			break;
		case 0x10: bpl(s, rel(s)); break;
		case 0x11:
			ea = indy(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x15:
			ea = zpx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x16:
			ea = zpx(s);
			rmw(s, ea, asl);
			break;
		case 0x18:
			imp(s);
			clc(s);
			break;
		case 0x19:
			ea = absy(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x1D:
			ea = absx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x1E:
			ea = absx(s);
			rmw(s, ea, asl);
			break;
		case 0x20:
			ea = abso(s);
			jsr(s, ea);
			break;
		case 0x21:
			ea = indx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x24:
			ea = zp(s);
			op_bit(s, get_ea(s, ea));
			break;
		case 0x25:
			ea = zp(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x26:
			ea = zp(s);
			rmw(s, ea, rol);
			break;
		case 0x28:
			imp(s);
			plp(s);
			break;
		case 0x29:
			ea = imm(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x2A: put_a(s, rol(s, get_a(s))); break;
		case 0x2C:
			ea = abso(s);
			op_bit(s, get_ea(s, ea));
			break;
		case 0x2D:
			ea = abso(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x2E:
			ea = abso(s);
			rmw(s, ea, rol);
			break;
		case 0x30: bmi(s, rel(s)); break;
		case 0x31:
			ea = indy(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x35:
			ea = zpx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x36:
			ea = zpx(s);
			rmw(s, ea, rol);
			break;
		case 0x38:
			imp(s);
			sec(s);
			break;
		case 0x39:
			ea = absy(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x3D:
			ea = absx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x3E:
			ea = absx(s);
			rmw(s, ea, rol);
			break;
		case 0x40:
			imp(s);
			rti(s);
			break;
		case 0x41:
			ea = indx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x45:
			ea = zp(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x46:
			ea = zp(s);
			rmw(s, ea, lsr);
			break;
		case 0x48:
			imp(s);
			pha(s);
			break;
		case 0x49:
			ea = imm(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x4A: put_a(s, lsr(s, get_a(s))); break;
		case 0x4C:
			ea = abso(s);
			jmp(s, ea);
			break;
		case 0x4D:
			ea = abso(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x4E:
			ea = abso(s);
			rmw(s, ea, lsr);
			break;
		case 0x50: bvc(s, rel(s)); break;
		case 0x51:
			ea = indy(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x55:
			ea = zpx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x56:
			ea = zpx(s);
			rmw(s, ea, lsr);
			break;
		case 0x58:
			imp(s);
			cli(s);
			break;
		case 0x59:
			ea = absy(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x5D:
			ea = absx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x5E:
			ea = absx(s);
			rmw(s, ea, lsr);
			break;
		case 0x60:
			imp(s);
			rts(s);
			break;
		case 0x61:
			ea = indx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x65:
			ea = zp(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x66:
			ea = zp(s);
			rmw(s, ea, ror);
			break;
		case 0x68:
			imp(s);
			pla(s);
			break;
		case 0x69:
			ea = imm(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x6A: put_a(s, ror(s, get_a(s))); break;
		case 0x6C:
			ea = ind(s);
			jmp(s, ea);
			break;
		case 0x6D:
			ea = abso(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x6E:
			ea = abso(s);
			rmw(s, ea, ror);
			break;
		case 0x70: bvs(s, rel(s)); break;
		case 0x71:
			ea = indy(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x75:
			ea = zpx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x76:
			ea = zpx(s);
			rmw(s, ea, ror);
			break;
		case 0x78:
			imp(s);
			sei(s);
			break;
		case 0x79:
			ea = absy(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x7D:
			ea = absx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x7E:
			ea = absx(s);
			rmw(s, ea, ror);
			break;
		case 0x81:
			ea = indx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x84:
			ea = zp(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x85:
			ea = zp(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x86:
			ea = zp(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x88:
			imp(s);
			dey(s);
			break;
		case 0x8A:
			imp(s);
			txa(s);
			break;
		case 0x8C:
			ea = abso(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x8D:
			ea = abso(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x8E:
			ea = abso(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x90: bcc(s, rel(s)); break;
		case 0x91:
			ea = indy(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x94:
			ea = zpx(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x95:
			ea = zpx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x96:
			ea = zpy(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x98:
			imp(s);
			tya(s);
			break;
		case 0x99:
			ea = absy(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x9A:
			imp(s);
			txs(s);
			break;
		case 0x9D:
			ea = absx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0xA0:
			ea = imm(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xA1:
			ea = indx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xA2:
			ea = imm(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xA4:
			ea = zp(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xA5:
			ea = zp(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xA6:
			ea = zp(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xA8:
			imp(s);
			tay(s);
			break;
		case 0xA9:
			ea = imm(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xAA:
			imp(s);
			tax(s);
			break;
		case 0xAC:
			ea = abso(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xAD:
			ea = abso(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xAE:
			ea = abso(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xB0: bcs(s, rel(s)); break;
		case 0xB1:
			ea = indy(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xB4:
			ea = zpx(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xB5:
			ea = zpx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xB6:
			ea = zpy(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xB8:
			imp(s);
			clv(s);
			break;
		case 0xB9:
			ea = absy(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xBA:
			imp(s);
			tsx(s);
			break;
		case 0xBC:
			ea = absx(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xBD:
			ea = absx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xBE:
			ea = absy(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xC0:
			ea = imm(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xC1:
			ea = indx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xC4:
			ea = zp(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xC5:
			ea = zp(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xC6:
			ea = zp(s);
			rmw(s, ea, dec);
			break;
		case 0xC8:
			imp(s);
			iny(s);
			break;
		case 0xC9:
			ea = imm(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xCA:
			imp(s);
			dex(s);
			break;
		case 0xCC:
			ea = abso(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xCD:
			ea = abso(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xCE:
			ea = abso(s);
			rmw(s, ea, dec);
			break;
		case 0xD0: bne(s, rel(s)); break;
		case 0xD1:
			ea = indy(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xD5:
			ea = zpx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xD6:
			ea = zpx(s);
			rmw(s, ea, dec);
			break;
		case 0xD8:
			imp(s);
			cld(s);
			break;
		case 0xD9:
			ea = absy(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xDD:
			ea = absx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xDE:
			ea = absx(s);
			rmw(s, ea, dec);
			break;
		case 0xE0:
			ea = imm(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xE1:
			ea = indx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xE4:
			ea = zp(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xE5:
			ea = zp(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xE6:
			ea = zp(s);
			rmw(s, ea, inc);
			break;
		case 0xE8:
			imp(s);
			inx(s);
			break;
		case 0xE9:
			ea = imm(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEB:
			ea = imm(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEC:
			ea = abso(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xED:
			ea = abso(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEE:
			ea = abso(s);
			rmw(s, ea, inc);
			break;
		case 0xF0: beq(s, rel(s)); break;
		case 0xF1:
			ea = indy(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xF5:
			ea = zpx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xF6:
			ea = zpx(s);
			rmw(s, ea, inc);
			break;
		case 0xF8:
			imp(s);
			sed(s);
			break;
		case 0xF9:
			ea = absy(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xFD:
			ea = absx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xFE:
			ea = absx(s);
			rmw(s, ea, inc);
			break;

		// --- Undocumented opcodes -------------------------------------------
		case 0x03: slo(s, indx(s)); break;
		case 0x07: slo(s, zp(s)); break;
		case 0x0F: slo(s, abso(s)); break;
		case 0x13: slo(s, indy(s)); break;
		case 0x17: slo(s, zpx(s)); break;
		case 0x1B: slo(s, absy(s)); break;
		case 0x1F: slo(s, absx(s)); break;

		case 0x23: rla(s, indx(s)); break;
		case 0x27: rla(s, zp(s)); break;
		case 0x2F: rla(s, abso(s)); break;
		case 0x33: rla(s, indy(s)); break;
		case 0x37: rla(s, zpx(s)); break;
		case 0x3B: rla(s, absy(s)); break;
		case 0x3F: rla(s, absx(s)); break;

		case 0x43: sre(s, indx(s)); break;
		case 0x47: sre(s, zp(s)); break;
		case 0x4F: sre(s, abso(s)); break;
		case 0x53: sre(s, indy(s)); break;
		case 0x57: sre(s, zpx(s)); break;
		case 0x5B: sre(s, absy(s)); break;
		case 0x5F: sre(s, absx(s)); break;

		case 0x63: rra(s, indx(s)); break;
		case 0x67: rra(s, zp(s)); break;
		case 0x6F: rra(s, abso(s)); break;
		case 0x73: rra(s, indy(s)); break;
		case 0x77: rra(s, zpx(s)); break;
		case 0x7B: rra(s, absy(s)); break;
		case 0x7F: rra(s, absx(s)); break;

		case 0x83: sax(s, indx(s)); break;
		case 0x87: sax(s, zp(s)); break;
		case 0x8F: sax(s, abso(s)); break;
		case 0x97: sax(s, zpy(s)); break;

		case 0xA3: lax(s, get_ea(s, indx(s))); break;
		case 0xA7: lax(s, get_ea(s, zp(s))); break;
		case 0xAF: lax(s, get_ea(s, abso(s))); break;
		case 0xB3: lax(s, get_ea(s, indy(s))); break;
		case 0xB7: lax(s, get_ea(s, zpy(s))); break;
		case 0xBF: lax(s, get_ea(s, absy(s))); break;

		case 0xC3: dcp(s, indx(s)); break;
		case 0xC7: dcp(s, zp(s)); break;
		case 0xCF: dcp(s, abso(s)); break;
		case 0xD3: dcp(s, indy(s)); break;
		case 0xD7: dcp(s, zpx(s)); break;
		case 0xDB: dcp(s, absy(s)); break;
		case 0xDF: dcp(s, absx(s)); break;

		case 0xE3: isc(s, indx(s)); break;
		case 0xE7: isc(s, zp(s)); break;
		case 0xEF: isc(s, abso(s)); break;
		case 0xF3: isc(s, indy(s)); break;
		case 0xF7: isc(s, zpx(s)); break;
		case 0xFB: isc(s, absy(s)); break;
		case 0xFF: isc(s, absx(s)); break;

		case 0x0B:
		case 0x2B: anc(s, get_ea(s, imm(s))); break;
		case 0x4B: alr(s, get_ea(s, imm(s))); break;
		case 0x6B: arr(s, get_ea(s, imm(s))); break;
		case 0x8B: ane(s, get_ea(s, imm(s))); break;
		case 0xAB: lxa(s, get_ea(s, imm(s))); break;
		case 0xCB: sbx(s, get_ea(s, imm(s))); break;
		case 0xBB: las(s, get_ea(s, absy(s))); break;

		case 0x93: sh_store(s, indy_base(s), s->cpu.y, s->cpu.a & s->cpu.x); break; // SHA (ind),Y
		case 0x9F: sh_store(s, abs_base(s), s->cpu.y, s->cpu.a & s->cpu.x); break;  // SHA abs,Y
		case 0x9E: sh_store(s, abs_base(s), s->cpu.y, s->cpu.x); break;             // SHX abs,Y
		case 0x9C: sh_store(s, abs_base(s), s->cpu.x, s->cpu.y); break;             // SHY abs,X
		case 0x9B:                                                                  // TAS abs,Y
			s->cpu.sp = s->cpu.a & s->cpu.x;
			sh_store(s, abs_base(s), s->cpu.y, s->cpu.sp);
			break;

			// clang-format off
		// NOPs that still read their operand, and so can touch I/O.
		case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
		case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: imm(s); break;
		case 0x04: case 0x44: case 0x64: get_ea(s, zp(s)); break;
		case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: get_ea(s, zpx(s)); break;
		case 0x0C: get_ea(s, abso(s)); break;
		case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: get_ea(s, absx(s)); break;
		// clang-format on
		default:
		{
			// What is left is JAM: the CPU stops. Leave the PC on the opcode so it
			// spins there, as the opcode table's zero length says.
			const int len = opcodes[opcode].length;
			s->cpu.pc = static_cast<uint16_t>(s->cpu.pc + (len == 0 ? -1 : len - 1));
			break;
		}
		}

		s->clock_ticks += static_cast<int32_t>(ticktable[opcode]);
		// 6502 read instructions with abs,X / abs,Y / (ind),Y addressing take one
		// extra cycle when the effective address crosses a page boundary.
		if (s->penalty_addr && penalty_op(opcode)) s->clock_ticks += 1;
		s->instruction_count++;
		s->step_hardware(static_cast<int32_t>(s->clock_ticks - cycles_before));
	}
}
