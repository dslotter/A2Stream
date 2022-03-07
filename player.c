/******************************************************************************

Copyright (c) 2022, Oliver Schmidt
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL OLIVER SCHMIDT BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

******************************************************************************/

#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "w5100.h"
#include "a2stream.h"

#include "player.h"

//
// This pulse-width modulated (PWM) digital-to-analog converter (DAC) uses a
// _pulse_ rate of 22.05kHz, simply because that's a common _sample_ rate and
// because DAC522 uses that _pulse_ rate successfully. However, DAC522 uses a
// _sample_ rate of 11.025kHz with two-times oversampling while this DAC uses
// a "true" sample rate of 22.05kHz.
//
// A pulse rate of 22.05kHz means 46 Apple II clock cycles per per pulse.
// Both DAC522 and this DAC use individual pulse generators for each pulse
// duty cycle and chain them via (modified) jumps to produce the desired pulse
// stream.
//
// Each 46 cycle pulse generator starts the PWM duty cycle by accessing the
// speaker (using 4 clock cycles) and ends with a jump (using 3 clock cycles).
// This means there are 39 clock cycles left to end the PWM duty cycle by
// accessing the speaker again (using again 4 clock cycles). So this allows for
// 36 different clock cycles to place that second speaker access on. These 36
// different duty cycles are generated by 36 different pulse generators which
// are numbered from 0 to 35. 
// 
// However, pulse generators 1 and 34 pose a special problem. Let's start with
// looking at generator 1:
//
// Generator 0 starts with
//   STA $C030  ; duty cycle start
//   STA $C030  ; duty cycle end
//   ...
//
// Generator 1 needs to place the duty cycle end exactly one machine cycle
// later but the 6502 doesn't have any one-cycle instruction to place between
// the two STA instructions.
//
// Generator 34 has a similiar issue:
//
// Generator 35 ends with
//   ...
//   STA $C030  ; duty cycle end
//   JMP NEXT   ; chain next generator
//
// Generator 34 needs to place the duty cylce end exactly one clock cycle
// earlier but the 6502 doesn't have any one-cycle instruction to place between
// the STA and the JMP instructions.
//
// DAC522 leaves those two issues aside and doesn't implement the 4 pulse
// generators 0, 1, 34 and 35 in the first place. Therefore it ends up with 32
// different pulse generators, meaning a straight 5-bit resolution.
//
// However, this DAC provides the maximum resolution by implementing all 36
// possible pulse generators.
//
// The generator 1 issue needs to be solved by replacing the second 4 clock
// cycle speaker access instruction with a 5 clock cycle instruction. Relying
// on 65C02 instructions, this can be done without relying on any CPU register
// content using the STA (ZP) instruction. So generator 1 starts with
//   STA $C030  ; duty cycle start
//   STA ($FA)  ; duty cycle end ($FA/$FB pointing to $C030)
//   ...
//
// The generator 34 issue needs a more complex solution. It requires the duty
// cycle to end 4 clock cycles before the pulse generator end. However, the
// 65(C)02 doesn't have any 4-cycle instruction that modifies the program
// counter. Therefore the end of generator 34 is prepended to _every_ generator
// allowing generator 34 to do the JMP _before_ the duty cycle end with
//   ...
//   JMP NEXT
//   STA $C030  ; 
//   NOP        ; these three instructions are prepended to all generators
//   NOP        ;
//

//
// The Uthernet II card requires the 6502 to do some work to initialize and
// finalize the reception of a block of data from the network. This work can
// _not_ be done within a single pulse generator. So in order to provide a
// glitch-free audio streaming, it is necessary to use a temporary buffer
// holding received samples. Therefore, every pulse generator has 3 independent
// tasks:
// 1.  Access the speaker twice according the duty cycle of generator
// 2.  Read the next sample from the buffer and modify the jump
// 3.1 Initialize data reception
// or
// 3.2 Write received samples to the buffer
// or
// 3.3 Finalize data reception
// or
// 3.4 Do something different 
//
// The different 3.x tasks require different types of pulse generators. There
// actually 10 different pulse generator types. Together with the individual
// pulse generators for the 36 different duty cycles this means a matrix of
// 10 * 36 pulse generators.
// The sample read from the buffer only modifies the jump target hi byte.
// Therefore all 36 individual pulse generators of a certain type need to be
// placed at the same offset in 36 individual memory pages. Fortunately the
// pulse generators are quite small so 5 types of pulse generators can share a
// single memory page - with each type at a different offset.
// 36 memory pages with 5 pulse generators on each page form a page set. So
// two page sets are enough to hold all 360 pulse generators. This means 18kB
// RAM.
//

//
// Performing 3 independent tasks as fast as possible asks for more CPU
// regsiters than the 6502 provides. Therefore the CPU stack is repurposed as
// sample buffer. This allows the S register to serve as an additional index
// register.
// The sample buffer is a ring buffer so the S register can be either used to
// write to the buffer via PHA (with reading from it via LDA $100,X) or to read
// from the buffer via PLA (with writing to it via STA $100,X).
// Given that PHA + LDA $100,X requires 7 cycles while STA $100,X + PLA
// requires 9 cycles, the former approach is in general superior.
// However, this DAC chooses the latter approach because reading from the
// buffer and modifying the jump needs to be interleaved with several different
// other tasks. And the 65C02 allows with PLX + STX to do just that without
// trashing the accumulator.
//

#define DTY_MAX 36    // duty maximum (5.x bit resolution)
#define CYC_MAX 46    // cycle maximum for pulse generator
#define GEN_MAX 43    // byte size maximum for pulse generator
#define GEN_END 5     // byte size of pulse generator 34 end
#define GEN_NUM 5     // number of pulse generators per page
#define SET_NUM 2     // number of pulse generator page sets
#define RW_SKEW 4     // byte skew between read and write

// According to the ProDOS 8 Technical Reference Manual Fig. A�3
// 'Zero Page Memory Map', zero page locations $FA-$FF are free.

#define SPKR_PTR 0xFA     // zp speaker pointer
#define VISU_PTR 0xFC     // zp visualization pointer

// Acccording to the ProDOS 8 Technical Note #18,
// an empty /RAM means that $1000-$BFFF are free.

#define RING_BUF 0x0100   // stack ring buffer
#define SAVE_BUF 0x1F00   // stack save buffer
#define PLAY_BUF 0x4000   // generated player

#define VISU_BUF 0x10D8   // $D8 > GEN_MAX * GEN_NUM !!!
#define VISU_MAX 140      // lo:$10D8-$1ED8 hi:$40D8-$BED8
#define VISU_L2H 13       // point to switch from lo to hi
#define VISU_NUM 39       // number of visualization bytes

#ifndef HAVE_ETH
#define MOCK_BUF 0xDF00   // mock ring buffer
#define NOT_USED 0x0300   // not used address
#endif

#define LEAVE 0xD400

#define HIRES_186 0x2BD0  // hires scanline 186
#define HIRES_187 0x2FD0  // hires scanline 187
#define HIRES_188 0x33D0  // hires scanline 188
#define HIRES_189 0x37D0  // hires scanline 189
#define HIRES_190 0x3BD0  // hires scanline 190
#define HIRES_191 0x3FD0  // hires scanline 191

#define write_main()  (*(uint8_t *)0xC004 = 0)
#define write_aux()   (*(uint8_t *)0xC005 = 0)
#define mix_off()     (*(uint8_t *)0xC052 = 0)
#define mix_on()      (*(uint8_t *)0xC053 = 0)

#define HIGH  0xC085  // W5100 address high byte
#define LOW   0xC086  // W5100 address low byte
#define DATA  0xC087  // W5100 data

#define SILENCE (PLAY_BUF + DTY_MAX / 2 * 0x0100)

#define LO(addr)  ((uint8_t)((uint16_t)(addr)   ))
#define HI(addr)  ((uint8_t)((uint16_t)(addr)>>8))

struct ins {
  uint8_t opc[5];   // for Bxx_JMP, see below
  uint8_t len;
  uint8_t cyc;
  bool    eth;      // Ethernet card access
};

#define BRK           {{0x00},                                 0, 0, false}  // only end marker
#define NOP           {{0xEA},                                 1, 2, false}
#define TAY           {{0xA8},                                 1, 2, false}
#define INC           {{0x1A},                                 1, 2, false}
#define INY           {{0xC8},                                 1, 2, false}
#define PLA           {{0x68},                                 1, 4, false}
#define PLX           {{0xFA},                                 1, 4, false}
#define BRA(disp)     {{0x80, (disp)},                         2, 3, false}
#define BNE_JMP(addr) {{0xD0, 0x03, 0x4C, LO(addr), HI(addr)}, 5, 3, false} // assume taken
#define BPL_JMP(addr) {{0x10, 0x03, 0x4C, LO(addr), HI(addr)}, 5, 3, false} // assume taken
#define JMP(addr)     {{0x4C, LO(addr), HI(addr)},             3, 3, false}
#define AND_IM(byte)  {{0x29, (byte)},                         2, 2, false}
#define ORA_IM(byte)  {{0x09, (byte)},                         2, 2, false}
#define LDA_IM(byte)  {{0xA9, (byte)},                         2, 2, false}
#define LDY_IM(byte)  {{0xA0, (byte)},                         2, 2, false}
#define STA_ZP(addr)  {{0x85, (addr)},                         2, 3, false}
#define LDA_A(addr)   {{0xAD, LO(addr), HI(addr)},             3, 4, false}
#define LDA_E(addr)   {{0xAD, LO(addr), HI(addr)},             3, 4, true}
#define LDY_E(addr)   {{0xAC, LO(addr), HI(addr)},             3, 4, true}
#define STA_A(addr)   {{0x8D, LO(addr), HI(addr)},             3, 4, false}
#define STX_A(addr)   {{0x8E, LO(addr), HI(addr)},             3, 4, false}
#define STA_E(addr)   {{0x8D, LO(addr), HI(addr)},             3, 4, true}
#define STY_E(addr)   {{0x8C, LO(addr), HI(addr)},             3, 4, true}
#define LDA_AY(addr)  {{0xB9, LO(addr), HI(addr)},             3, 4, false}
#define STA_AY(addr)  {{0x99, LO(addr), HI(addr)},             3, 5, false}
#define LDA_I(addr)   {{0xB2, (addr)},                         2, 5, false}
#define LDA_IY(addr)  {{0xB1, (addr)},                         2, 5, false}
#define STA_I(addr)   {{0x92, (addr)},                         2, 5, false}

static struct ins nop =      NOP;
static struct ins bra_nop =  BRA(0x00);
static struct ins spkr_a =   STA_A(0xC030);    // access speaker directly
static struct ins spkr_i =   STA_I(SPKR_PTR);  // access speaker via pointer
static struct ins plx =      PLX;              // pull next sample from buffer
static struct ins stx =      STX_A(0x0000);    // store jump address high byte
static struct ins jmp =      JMP(0x0000);      // jump to modified address

static struct ins pulse_34[] = {
  STA_A(0xC030),    // |
  NOP,              // |- duty end for generator 34
  NOP,              // |
  STA_A(0xC030),    // duty start
  PLX,              // pull next sample from buffer
  STX_A(0x0000),    // store jump address high byte
  BRK
};

static struct ins prolog_1[] = {
  LDA_A(0xC000),    // keyboard
  BPL_JMP(LEAVE),   // no key pressed
  LDA_IM(0x04),     // socket 0
  STA_E(HIGH),
  LDY_IM(0x26),     // received size register
  STY_E(LOW),
  BRK
};

static struct ins prolog_2[] = {
  LDA_E(DATA),      // high byte
  #ifdef HAVE_ETH
  BNE_JMP(LEAVE),   // at least one page available
  #endif
  LDY_IM(0x28),     // read pointer register
  STY_E(LOW),
  LDA_E(DATA),      // high byte
  LDY_E(DATA),      // low byte
  BRK
};

static struct ins prolog_3[] = {
  AND_IM(0x1F),     // socket 0 rx memory size
  ORA_IM(0x60),     // socket 0 rx memory addr
  STA_E(HIGH),      // read addr high
  STY_E(LOW),       // read addr low
  LDY_IM(RW_SKEW-1),
  BRK
};

static struct ins transf_1[] = {
  INY,
  #ifdef HAVE_ETH
  LDA_E(DATA),
  #else
  LDA_AY(MOCK_BUF),
  #endif
  STA_AY(RING_BUF),
  INY,
  BRK
};

static struct ins transf_2[] = {
  #ifdef HAVE_ETH
  LDA_E(DATA),
  #else
  LDA_AY(MOCK_BUF),
  #endif
  STA_AY(RING_BUF),
  INY,
  #ifdef HAVE_ETH
  LDA_E(DATA),
  #else
  LDA_AY(MOCK_BUF),
  #endif
  STA_AY(RING_BUF),
  BRK
};

static struct ins epilog_1[] = {
  LDA_IM(0x04),     // socket 0
  STA_E(HIGH),
  LDY_IM(0x28),     // read pointer register
  STY_E(LOW),
  LDA_E(DATA),      // high byte
  STY_E(LOW),
  BRK
};

static struct ins epilog_2[] = {
  INC,              // commit one page
  STA_E(DATA),      // high byte
  LDY_IM(0x01),     // command register
  STY_E(LOW),
  LDA_IM(0x40),     // RECV
  STA_E(DATA),
  BRK
};

static struct ins init_vis[] = {
  PLA,              // visualization slot
  STA_ZP(VISU_PTR+1),
  LDA_I(VISU_PTR),
  TAY,
  STA_AY(0xC054),   // activate page
  LDY_IM(0x01),
  BRK
};

static struct ins visual_1[] = {
  LDA_IY(VISU_PTR),
  STA_AY(HIRES_186),
  STA_AY(HIRES_187),
  STA_AY(HIRES_188),
  BRK
};

static struct ins visual_2[] = {
  STA_AY(HIRES_189),
  STA_AY(HIRES_190),
  STA_AY(HIRES_191),
  INY,
  BRK
};

// place banking switching code in language card
#pragma code-name (push, "LC")

/* not static */ void leave(void)
{
  // assert page 1
  asm volatile ("sta $C054");

  // restore stack
  asm volatile ("ldx %w", SAVE_BUF);
  asm volatile ("txs");
loop:
  asm volatile ("lda %w,x", SAVE_BUF);
  asm volatile ("sta %w,x", RING_BUF);
  asm volatile ("inx");
  asm volatile ("bne %g", loop);

  // switch to MAIN
  asm volatile ("stz $C002");
  asm volatile ("stz $C004");
}

static void enter(void)
{
  // switch to AUX
  asm volatile ("stz $C003");
  asm volatile ("stz $C005");

  // save stack
  asm volatile ("tsx");
  asm volatile ("stx %w", SAVE_BUF);
loop:
  asm volatile ("lda %w,x", RING_BUF);
  asm volatile ("sta %w,x", SAVE_BUF);
  asm volatile ("inx");
  asm volatile ("bne %g", loop);

  // init ring buffer
  asm volatile ("ldx #%b", RW_SKEW-1);
  asm volatile ("txs");
  asm volatile ("lda #%b", HI(SILENCE));
  asm volatile ("pha");
  asm volatile ("pha");
  asm volatile ("pha");
  asm volatile ("pha");

  // start player
  asm volatile ("jmp %w", SILENCE);
}

#pragma code-name (pop)

//
// ================|================
//    page set 0   |   page set 1
// ================|================
//                 |
//
//       prolog_1 <--------------+
//          |                    |
//          v                    |
//       prolog_2                |
//          |                    |
//          v                    |
//       prolog_3                |
//          |                    |
//          v                    |
//   +-> transf_1 --> epilog_1   |
//   |      |            |       |
//   |      v            v       |
//   |   transf_2     epilog_2   |
//   |      |            |       |
//   +------+            v       |
//                    init_vis   |
//                       |       |
//                       v       |
//                +-> visual_1   |
//                |      |       |
//                |      v       |
//                |   visual_2 --+
//                |      |
//                +------+
//
// Notes:
// 
// The pulse generators are chained via (modified) jumps. The jump target hi
// bytes are received from the server while the jump target lo bytes are static.
// 
// Beside determining the pulse width of the next sample, the jump target hi
// bytes decide, if page set 0 or page set 1 is used for the next sample.
// 
// The program flow within each page set as shown above is defined by the jump
// target lo bytes.
// 
// The flow of both page sets falls into an endless loop. By switching to the
// other page set, the server has the player break out of that loop.
//
// Pro: The player doesn't need any loop variables whatsoever.
// Con: The server needs to know when to break each of the loops.
//

static struct flow {
  struct ins *ins;
  uint8_t    nxt; // index of instructions for next pulse generator
} flow[SET_NUM][GEN_NUM] = {{
  {prolog_1, 3},  // 0
  {transf_2, 2},  // 1 - must match epilog_1 index to allow to switch there !!!
  {transf_1, 1},  // 2
  {prolog_2, 4},  // 3
  {prolog_3, 2}   // 4
},{
  {visual_1, 4},  // 0 - must match prolog_1 index to allow to switch there !!!
  {epilog_1, 2},  // 1
  {epilog_2, 3},  // 2
  {init_vis, 0},  // 3
  {visual_2, 0},  // 4
}};

static void fix_eth(register struct ins *ins, uint8_t ofs)
{
  while (ins->opc[0])
  {
    if (ins->eth)
    {
      ins->opc[1] |= ofs;
    }
    ++ins;
  }
}

enum nxt {pull, store, done};

static void gen_pulse(register uint8_t *ptr, register struct ins *ins,
                      register uint8_t dty, uint8_t low)
{
  // aggressive loop unrolling here... 

#ifndef NDEBUG
  uint8_t *org = ptr;
#endif
  register uint8_t cyc;
  enum nxt nxt;
  uint16_t *loc;

  // put duty end for generator 34
  assert(spkr_a.len == 3);
  assert(nop.len    == 1);
  write_aux();
  *ptr++ = spkr_a.opc[0];
  *ptr++ = spkr_a.opc[1];
  *ptr++ = spkr_a.opc[2];
  *ptr++ = nop.opc[0];
  *ptr++ = nop.opc[0];
  write_main();

  // put duty start
  assert(spkr_a.len == 3);
  write_aux();
  *ptr++ = spkr_a.opc[0];
  *ptr++ = spkr_a.opc[1];
  *ptr++ = spkr_a.opc[2];
  write_main();
  cyc = spkr_a.cyc;

  dty += spkr_a.cyc;      // minimal duty
  nxt = pull;

  while (cyc + jmp.cyc < CYC_MAX)
  {
    // put normal duty end if no cycle left
    if (cyc == dty)
    {
      assert(spkr_a.len == 3);
      write_aux();
      *ptr++ = spkr_a.opc[0];
      *ptr++ = spkr_a.opc[1];
      *ptr++ = spkr_a.opc[2];
      write_main();
      cyc += spkr_a.cyc;
      continue;
    }

    // put streched duty end if one cycle left
    if (cyc + 1 == dty)
    {
      assert(spkr_i.len == 2);
      write_aux();
      *ptr++ = spkr_i.opc[0];
      *ptr++ = spkr_i.opc[1];
      write_main();
      cyc += spkr_i.cyc;
      continue;
    }

    // put pulling next sample from buffer as soon as possible to
    // avoid trashing N and Z flags used by arbitrary instructions
    if (nxt == pull &&
        (cyc + plx.cyc <= dty ||    // instruction fits before duty end
         cyc            > dty))     // already after duty end
    {
      assert(plx.len == 1);
      write_aux();
      *ptr++ = plx.opc[0];
      write_main();
      cyc += plx.cyc;
      ++nxt;
      continue;
    }

    // put storing jump address high byte using next sample
    if (nxt == store &&
        (cyc + stx.cyc <= dty ||    // instruction fits before duty end
         cyc            > dty))     // already after duty end
    {
      assert(stx.len == 3);
      write_aux();
      *ptr++ = stx.opc[0];
      write_main();
      loc = (uint16_t *)ptr;    // save location until address is known
      ptr += 2;
      cyc += stx.cyc;
      ++nxt;
      continue;
    }

    // now put arbitrary instructions
    if (ins->opc[0] &&              // still instructions left to put
        (cyc + ins->cyc <= dty ||   // instruction fits before duty end
         cyc             > dty))    // already after duty end
    {
      uint8_t pos;
      for (pos = 0; pos < ins->len; ++pos)
      {
        write_aux();
        *ptr++ = ins->opc[pos];
        write_main();
      }
      cyc += ins++->cyc;
      continue;
    }

    // if number of cycles to fill is odd then put a BRA
    if (cyc < dty && (dty - cyc)     % 2 ||   // before duty end
        cyc > dty && (cyc - jmp.cyc) % 2)     // after  duty end
    {
      assert(bra_nop.len == 2);
      write_aux();
      *ptr++ = bra_nop.opc[0];
      *ptr++ = bra_nop.opc[1];
      write_main();
      cyc += bra_nop.cyc;
      continue;
    }

    assert(nop.len == 1);
    write_aux();
    *ptr++ = nop.opc[0];
    write_main();
    cyc += nop.cyc;
  }

  assert(jmp.len == 3);
  write_aux();
  *ptr++ = jmp.opc[0];
  *ptr++ = low;
  write_main();
  cyc += jmp.cyc;

  // set absolute location which stores jump address high byte
  write_aux();
  *loc = (uint16_t)ptr;
  write_main();

#ifndef NDEBUG
  printf("loc:%p dty:%02d len:%02d\n",
          org, dty - spkr_a.cyc, ptr - org);
#endif
  assert(cyc == CYC_MAX);       // no cycle count overshoot
  assert(ptr - org < GEN_MAX);  // no byte length overshoot
  assert(nxt == done);          // no next sample work left
  assert(!ins->opc[0]);         // no instruction left
}

static void gen_pulse_34(register uint8_t *ptr, register struct ins *ins,
                         register uint8_t low)
{
#ifndef NDEBUG
  uint8_t *org = ptr;
#endif
  register uint8_t cyc;
  struct ins *ins_34;
  uint16_t *loc;

  cyc = 0;
  ins_34 = pulse_34;

  // first put pulse_34 instructions
  while (ins_34->opc[0])      // still instructions left to put
  {
    uint8_t pos;
    for (pos = 0; pos < ins_34->len; ++pos)
    {
      write_aux();
      *ptr++ = ins_34->opc[pos];
      write_main();
    }
    cyc += ins_34++->cyc;
  }

  // storing jump address high byte is last instruction in pulse_34 !!!
  loc = (uint16_t *)ptr - 1;  // save location until address is known

  // now put arbitrary instructions
  while (ins->opc[0])         // still instructions left to put
  {
    uint8_t pos;
    for (pos = 0; pos < ins->len; ++pos)
    {
      write_aux();
      *ptr++ = ins->opc[pos];
      write_main();
    }
    cyc += ins++->cyc;
  }

  // if number of cycles to fill is odd then put a BRA
  if ((cyc - jmp.cyc) % 2)
  {
    assert(bra_nop.len == 2);
    write_aux();
    *ptr++ = bra_nop.opc[0];
    *ptr++ = bra_nop.opc[1];
    write_main();
    cyc += bra_nop.cyc;
  }

  while (cyc + jmp.cyc < CYC_MAX)
  {
    assert(nop.len == 1);
    write_aux();
    *ptr++ = nop.opc[0];
    write_main();
    cyc += nop.cyc;
  }

  assert(jmp.len == 3);
  write_aux();
  *ptr++ = jmp.opc[0];
  *ptr++ = low;
  cyc += jmp.cyc;
  write_main();

  // set absolute location which stores jump address high byte
  write_aux();
  *loc = (uint16_t)ptr;
  write_main();

#ifndef NDEBUG
  printf("loc:%p dty:34 len:%02d\n", org, ptr - org);
#endif
  assert(cyc == CYC_MAX);       // no cycle count overshoot
  assert(ptr - org < GEN_MAX);  // no byte length overshoot
}

static const char spin[] = {'/', '-', '\\', '|'};

void gen_player(uint8_t e_ini, bool t_out)
{
  uint8_t e_ofs = e_ini << 4;
  uint8_t s, g, d;

  assert(&leave == (void(*)(void))LEAVE);

  cputs("Generating player ");

  if (t_out)
  {
    spkr_a.opc[1] = 0x20; // switch from speaker (0xC030) to tape out (0xC020)
  }
  *(uint16_t *)SPKR_PTR = *(uint16_t *)&spkr_a.opc[1];
  *(uint16_t *)VISU_PTR = VISU_BUF;

  for (s = 0; s < SET_NUM; ++s)
  {
    uint8_t *s_ptr = (uint8_t *)PLAY_BUF + s * DTY_MAX * 0x0100;

    for (g = 0; g < GEN_NUM; ++g)
    {
      uint8_t *g_ptr = s_ptr + g * GEN_MAX;
      struct flow *f = &flow[s][g];
      uint8_t x = wherex();

      if (f->ins)
      {
        fix_eth(f->ins, e_ofs);
        for (d = 0; d < DTY_MAX; ++d)
        {
          cputc(spin[d % sizeof(spin)]);
          gotox(x);

          if (d == 34)
          {
            gen_pulse_34(g_ptr + d * 0x0100, f->ins, f->nxt * GEN_MAX);
          }
          else
          {
            gen_pulse(g_ptr + d * 0x0100, f->ins, d, f->nxt * GEN_MAX + GEN_END);
          }
        }
      }
      cputc('.');
    }
  }

  #ifndef HAVE_ETH
  *(uint8_t *)NOT_USED = *(uint8_t *)0xC081;
  *(uint8_t *)NOT_USED = *(uint8_t *)0xC081;
  memset((uint8_t *)MOCK_BUF,        HI(SILENCE),           0x0100);
  memset((uint8_t *)MOCK_BUF + 0xAB, HI(SILENCE) + DTY_MAX, 0x0050);
  *(uint8_t *)(MOCK_BUF + 0xAF) = HI(VISU_BUF);
  *(uint8_t *)NOT_USED = *(uint8_t *)0xC080;
  #endif
}

enum state {playing, pausing, waiting};

void play(void)
{
  uint8_t cya;
  enum state state = playing;

  {
    uint8_t v;
    uint8_t *v_ptr = (uint8_t *)VISU_BUF;

    for (v = 0; v < VISU_MAX; ++v)
    {
      if (!load(v_ptr, VISU_NUM, true))
      {
        w5100_disconnect();
        return;
      }
      if (v == VISU_L2H)
      {
        v_ptr = (uint8_t *)VISU_BUF + 0x3000;
      }
      else
      {
        v_ptr += 0x0100;
      }
    }
  }

  #ifndef HAVE_ETH
  write_aux();
  *(uint8_t *)VISU_BUF = 0;
  write_main();
  #endif

  if (get_ostype() & APPLE_IIGS)
  {
    cya = *(uint8_t *)0xC036;
    *(uint8_t *)0xC036 &= 0b01111111; // set normal speed
  }

  #ifdef HAVE_ETH
  while (w5100_connected())
  #else
  while (true)
  #endif
  {
    if (kbhit())
    {
      if (cgetc() == CH_ESC)
      {
        w5100_disconnect();
        return;
      }
      if (state == pausing)
      {
        state = playing;
      }
      else
      {
        state = pausing;
      }
    }
    #ifdef HAVE_ETH
    if (state != pausing)
    {
      if (w5100_receive_request() >= 0x0100)
      {
        state = playing;
      }
      else
      {
        state = waiting;
      }
    }
    #endif
    if (state == playing)
    {
      mix_off();
      enter();
    }
    else
    {
      mix_on();
      cputsxy(35, 22, state == pausing ? "Paus"
                                       : "Wait");
    }
  }

  if (get_ostype() & APPLE_IIGS)
  {
    *(uint8_t *)0xC036 = cya;
  }
}
