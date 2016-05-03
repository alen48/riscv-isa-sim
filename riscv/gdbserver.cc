#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

#include "disasm.h"
#include "sim.h"
#include "gdbserver.h"
#include "mmu.h"

#define C_EBREAK        0x9002
#define EBREAK          0x00100073

//////////////////////////////////////// Utility Functions

void die(const char* msg)
{
  fprintf(stderr, "gdbserver code died: %s\n", msg);
  abort();
}

// gdb's register list is defined in riscv_gdb_reg_names gdb/riscv-tdep.c in
// its source tree. We must interpret the numbers the same here.
enum {
  REG_XPR0 = 0,
  REG_XPR31 = 31,
  REG_PC = 32,
  REG_FPR0 = 33,
  REG_FPR31 = 64,
  REG_CSR0 = 65,
  REG_CSR4095 = 4160,
  REG_END = 4161
};

//////////////////////////////////////// Functions to generate RISC-V opcodes.

// TODO: Does this already exist somewhere?

// Using regnames.cc as source. The RVG Calling Convention of the 2.0 RISC-V
// spec says it should be 2 and 3.
#define S0      8
#define S1      9
static uint32_t bits(uint32_t value, unsigned int hi, unsigned int lo) {
  return (value >> lo) & ((1 << (hi+1-lo)) - 1);
}

static uint32_t bit(uint32_t value, unsigned int b) {
  return (value >> b) & 1;
}

static uint32_t jal(unsigned int rd, uint32_t imm) {
  return (bit(imm, 20) << 31) |
    (bits(imm, 10, 1) << 21) |
    (bit(imm, 11) << 20) |
    (bits(imm, 19, 12) << 12) |
    (rd << 7) |
    MATCH_JAL;
}

static uint32_t csrsi(unsigned int csr, uint8_t imm) {
  return (csr << 20) |
    (bits(imm, 4, 0) << 15) |
    MATCH_CSRRSI;
}

static uint32_t csrci(unsigned int csr, uint8_t imm) {
  return (csr << 20) |
    (bits(imm, 4, 0) << 15) |
    MATCH_CSRRCI;
}

static uint32_t csrr(unsigned int rd, unsigned int csr) {
  return (csr << 20) | (rd << 7) | MATCH_CSRRS;
}

static uint32_t csrw(unsigned int source, unsigned int csr) {
  return (csr << 20) | (source << 15) | MATCH_CSRRW;
}

static uint32_t sb(unsigned int src, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 5) << 25) |
    (src << 20) |
    (base << 15) |
    (bits(offset, 4, 0) << 7) |
    MATCH_SB;
}

static uint32_t sh(unsigned int src, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 5) << 25) |
    (src << 20) |
    (base << 15) |
    (bits(offset, 4, 0) << 7) |
    MATCH_SH;
}

static uint32_t sw(unsigned int src, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 5) << 25) |
    (src << 20) |
    (base << 15) |
    (bits(offset, 4, 0) << 7) |
    MATCH_SW;
}

static uint32_t sd(unsigned int src, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 5) << 25) |
    (bits(src, 4, 0) << 20) |
    (base << 15) |
    (bits(offset, 4, 0) << 7) |
    MATCH_SD;
}

static uint32_t ld(unsigned int rd, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 0) << 20) |
    (base << 15) |
    (bits(rd, 4, 0) << 7) |
    MATCH_LD;
}

static uint32_t lw(unsigned int rd, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 0) << 20) |
    (base << 15) |
    (bits(rd, 4, 0) << 7) |
    MATCH_LW;
}

static uint32_t lh(unsigned int rd, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 0) << 20) |
    (base << 15) |
    (bits(rd, 4, 0) << 7) |
    MATCH_LH;
}

static uint32_t lb(unsigned int rd, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 0) << 20) |
    (base << 15) |
    (bits(rd, 4, 0) << 7) |
    MATCH_LB;
}

static uint32_t fsd(unsigned int src, unsigned int base, uint16_t offset)
{
  return (bits(offset, 11, 5) << 25) |
    (bits(src, 4, 0) << 20) |
    (base << 15) |
    (bits(offset, 4, 0) << 7) |
    MATCH_FSD;
}

static uint32_t addi(unsigned int dest, unsigned int src, uint16_t imm)
{
  return (bits(imm, 11, 0) << 20) |
    (src << 15) |
    (dest << 7) |
    MATCH_ADDI;
}

static uint32_t nop()
{
  return addi(0, 0, 0);
}

template <typename T>
unsigned int circular_buffer_t<T>::size() const
{
  if (end >= start)
    return end - start;
  else
    return end + capacity - start;
}

template <typename T>
void circular_buffer_t<T>::consume(unsigned int bytes)
{
  start = (start + bytes) % capacity;
}

template <typename T>
unsigned int circular_buffer_t<T>::contiguous_empty_size() const
{
  if (end >= start)
    if (start == 0)
      return capacity - end - 1;
    else
      return capacity - end;
  else
    return start - end - 1;
}

template <typename T>
unsigned int circular_buffer_t<T>::contiguous_data_size() const
{
  if (end >= start)
    return end - start;
  else
    return capacity - start;
}

template <typename T>
void circular_buffer_t<T>::data_added(unsigned int bytes)
{
  end += bytes;
  assert(end <= capacity);
  if (end == capacity)
    end = 0;
}

template <typename T>
void circular_buffer_t<T>::reset()
{
  start = 0;
  end = 0;
}

template <typename T>
void circular_buffer_t<T>::append(const T *src, unsigned int count)
{
  unsigned int copy = std::min(count, contiguous_empty_size());
  memcpy(contiguous_empty(), src, copy * sizeof(T));
  data_added(copy);
  count -= copy;
  if (count > 0) {
    assert(count < contiguous_empty_size());
    memcpy(contiguous_empty(), src, count * sizeof(T));
    data_added(count);
  }
}

////////////////////////////// Debug Operations

class halt_op_t : public operation_t
{
  public:
    halt_op_t(gdbserver_t& gdbserver) : operation_t(gdbserver) {};

    bool perform_step(unsigned int step) {
      switch (step) {
        case 0:
          // TODO: For now we just assume the target is 64-bit.
          gs.write_debug_ram(0, csrsi(DCSR_ADDRESS, DCSR_HALT_MASK));
          gs.write_debug_ram(1, csrr(S0, DPC_ADDRESS));
          gs.write_debug_ram(2, sd(S0, 0, (uint16_t) DEBUG_RAM_START));
          gs.write_debug_ram(3, csrr(S0, CSR_MBADADDR));
          gs.write_debug_ram(4, sd(S0, 0, (uint16_t) DEBUG_RAM_START + 8));
          gs.write_debug_ram(5, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*5))));
          gs.set_interrupt(0);
          // We could read mcause here as well, but only on 64-bit targets. I'm
          // trying to keep The patterns here usable for 32-bit ISAs as well. (On a
          // 32-bit ISA 8 words are required, while the minimum Debug RAM size is 7
          // words.)

        case 1:
          gs.saved_dpc = ((uint64_t) gs.read_debug_ram(1) << 32) | gs.read_debug_ram(0);
          gs.saved_mbadaddr = ((uint64_t) gs.read_debug_ram(3) << 32) | gs.read_debug_ram(2);

          gs.write_debug_ram(0, csrr(S0, CSR_MCAUSE));
          gs.write_debug_ram(1, sd(S0, 0, (uint16_t) DEBUG_RAM_START + 0));
          gs.write_debug_ram(2, csrr(S0, CSR_MSTATUS));
          gs.write_debug_ram(3, sd(S0, 0, (uint16_t) DEBUG_RAM_START + 8));
          gs.write_debug_ram(4, csrr(S0, CSR_DCSR));
          gs.write_debug_ram(5, sd(S0, 0, (uint16_t) DEBUG_RAM_START + 16));
          gs.write_debug_ram(6, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*6))));
          gs.set_interrupt(0);

        case 2:
          gs.saved_mcause = ((uint64_t) gs.read_debug_ram(1) << 32) | gs.read_debug_ram(0);
          gs.saved_mstatus = ((uint64_t) gs.read_debug_ram(3) << 32) | gs.read_debug_ram(2);
          gs.dcsr = ((uint64_t) gs.read_debug_ram(5) << 32) | gs.read_debug_ram(4);
          return true;
      }

      return false;
    }
};

class continue_op_t : public operation_t
{
  public:
    continue_op_t(gdbserver_t& gdbserver) : operation_t(gdbserver) {};

    bool perform_step(unsigned int step) {
      switch (step) {
        case 0:
          gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START+16));
          gs.write_debug_ram(1, csrw(S0, DPC_ADDRESS));
          gs.write_debug_ram(2, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*2))));
          gs.write_debug_ram(4, gs.saved_dpc);
          gs.write_debug_ram(5, gs.saved_dpc >> 32);
          gs.set_interrupt(0);

        case 1:
          gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START+16));
          gs.write_debug_ram(1, csrw(S0, CSR_MBADADDR));
          gs.write_debug_ram(2, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*2))));
          gs.write_debug_ram(4, gs.saved_mbadaddr);
          gs.write_debug_ram(5, gs.saved_mbadaddr >> 32);
          gs.set_interrupt(0);

        case 2:
          gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START+16));
          gs.write_debug_ram(1, csrw(S0, CSR_MSTATUS));
          gs.write_debug_ram(2, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*2))));
          gs.write_debug_ram(4, gs.saved_mstatus);
          gs.write_debug_ram(5, gs.saved_mstatus >> 32);
          gs.set_interrupt(0);

        case 3:
          gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START+16));
          gs.write_debug_ram(1, csrw(S0, CSR_MCAUSE));
          gs.write_debug_ram(2, csrci(DCSR_ADDRESS, DCSR_HALT_MASK));
          gs.write_debug_ram(3, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*3))));
          gs.write_debug_ram(4, gs.saved_mcause);
          gs.write_debug_ram(5, gs.saved_mcause >> 32);
          gs.set_interrupt(0);
          return true;
      }
      return false;
    }
};

class general_registers_read_op_t : public operation_t
{
  // Register order that gdb expects is:
  //   "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
  //   "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
  //   "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
  //   "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31",

  // Each byte of register data is described by two hex digits. The bytes with
  // the register are transmitted in target byte order. The size of each
  // register and their position within the ‘g’ packet are determined by the
  // gdb internal gdbarch functions DEPRECATED_REGISTER_RAW_SIZE and
  // gdbarch_register_name.

  public:
    general_registers_read_op_t(gdbserver_t& gdbserver) :
      operation_t(gdbserver) {};

    bool perform_step(unsigned int step)
    {
      if (step == 0) {
        gs.start_packet();

        // x0 is always zero.
        gs.send((reg_t) 0);

        gs.write_debug_ram(0, sd(1, 0, (uint16_t) DEBUG_RAM_START + 16));
        gs.write_debug_ram(1, sd(2, 0, (uint16_t) DEBUG_RAM_START + 0));
        gs.write_debug_ram(2, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*2))));
        gs.set_interrupt(0);
        return false;
      }

      gs.send(((uint64_t) gs.read_debug_ram(5) << 32) | gs.read_debug_ram(4));
      if (step >= 16) {
        gs.end_packet();
        return true;
      }

      gs.send(((uint64_t) gs.read_debug_ram(1) << 32) | gs.read_debug_ram(0));

      unsigned int current_reg = 2 * step - 1;
      unsigned int i = 0;
      if (current_reg == S1) {
        gs.write_debug_ram(i++, ld(S1, 0, (uint16_t) DEBUG_RAM_END - 8));
      }
      gs.write_debug_ram(i++, sd(current_reg, 0, (uint16_t) DEBUG_RAM_START + 16));
      if (current_reg + 1 == S0) {
        gs.write_debug_ram(i++, csrr(S0, CSR_DSCRATCH));
      }
      gs.write_debug_ram(i++, sd(current_reg+1, 0, (uint16_t) DEBUG_RAM_START + 0));
      gs.write_debug_ram(i, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*i))));
      gs.set_interrupt(0);

      return false;
    }
};

class register_read_op_t : public operation_t
{
  public:
    register_read_op_t(gdbserver_t& gdbserver, unsigned int reg) :
      operation_t(gdbserver), reg(reg) {};

    bool perform_step(unsigned int step)
    {
      switch (step) {
        case 0:
          if (reg >= REG_XPR0 && reg <= REG_XPR31) {
            die("handle_register_read");
            // send(p->state.XPR[reg - REG_XPR0]);
          } else if (reg == REG_PC) {
            gs.start_packet();
            gs.send(gs.saved_dpc);
            gs.end_packet();
            return true;
          } else if (reg >= REG_FPR0 && reg <= REG_FPR31) {
            // send(p->state.FPR[reg - REG_FPR0]);
            gs.write_debug_ram(0, fsd(reg - REG_FPR0, 0, (uint16_t) DEBUG_RAM_START + 16));
            gs.write_debug_ram(1, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*1))));
          } else if (reg == REG_CSR0 + CSR_MBADADDR) {
            gs.start_packet();
            gs.send(gs.saved_mbadaddr);
            gs.end_packet();
            return true;
          } else if (reg == REG_CSR0 + CSR_MCAUSE) {
            gs.start_packet();
            gs.send(gs.saved_mcause);
            gs.end_packet();
            return true;
          } else if (reg >= REG_CSR0 && reg <= REG_CSR4095) {
            gs.write_debug_ram(0, csrr(S0, reg - REG_CSR0));
            gs.write_debug_ram(1, sd(S0, 0, (uint16_t) DEBUG_RAM_START + 16));
            gs.write_debug_ram(2, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*2))));
            // If we hit an exception reading the CSR, we'll end up returning ~0 as
            // the register's value, which is what we want. (Right?)
            gs.write_debug_ram(4, 0xffffffff);
            gs.write_debug_ram(5, 0xffffffff);
          } else {
            gs.send_packet("E02");
            return true;
          }
          gs.set_interrupt(0);

        case 1:
          gs.start_packet();
          gs.send(((uint64_t) gs.read_debug_ram(5) << 32) | gs.read_debug_ram(4));
          gs.end_packet();
          return true;
      }
      return false;
    }

  private:
    unsigned int reg;
};

class memory_read_op_t : public operation_t
{
  public:
    memory_read_op_t(gdbserver_t& gdbserver, reg_t addr, unsigned int length) :
      operation_t(gdbserver), addr(addr), length(length) {};

    bool perform_step(unsigned int step)
    {
      if (step == 0) {
        // address goes in S0
        access_size = (addr % length);
        if (access_size == 0)
          access_size = length;

        gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START + 16));
        switch (access_size) {
          case 1:
            gs.write_debug_ram(1, lb(S1, S0, 0));
            break;
          case 2:
            gs.write_debug_ram(1, lh(S1, S0, 0));
            break;
          case 4:
            gs.write_debug_ram(1, lw(S1, S0, 0));
            break;
          case 8:
            gs.write_debug_ram(1, ld(S1, S0, 0));
            break;
          default:
            gs.send_packet("E12");
            return true;
        }
        gs.write_debug_ram(2, sd(S1, 0, (uint16_t) DEBUG_RAM_START + 24));
        gs.write_debug_ram(3, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*3))));
        gs.write_debug_ram(4, addr);
        gs.write_debug_ram(5, addr >> 32);
        gs.set_interrupt(0);

        gs.start_packet();
        return false;
      }

      char buffer[3];
      reg_t value = ((uint64_t) gs.read_debug_ram(7) << 32) | gs.read_debug_ram(6);
      for (unsigned int i = 0; i < access_size; i++) {
        sprintf(buffer, "%02x", (unsigned int) (value & 0xff));
        gs.send(buffer);
        value >>= 8;
      }
      length -= access_size;
      addr += access_size;

      if (length == 0) {
        gs.end_packet();
        return true;
      } else {
        gs.write_debug_ram(4, addr);
        gs.write_debug_ram(5, addr >> 32);
        gs.set_interrupt(0);
        return false;
      }
    }

  private:
    reg_t addr;
    unsigned int length;
    unsigned int access_size;
};

class memory_write_op_t : public operation_t
{
  public:
    memory_write_op_t(gdbserver_t& gdbserver, reg_t addr, unsigned int length,
        unsigned char *data) :
      operation_t(gdbserver), addr(addr), offset(0), length(length), data(data) {};

    ~memory_write_op_t() {
      delete[] data;
    }

    bool perform_step(unsigned int step)
    {
      if (step == 0) {
        // address goes in S0
        access_size = (addr % length);
        if (access_size == 0)
          access_size = length;

        gs.write_debug_ram(0, ld(S0, 0, (uint16_t) DEBUG_RAM_START + 16));
        switch (access_size) {
          case 1:
            gs.write_debug_ram(1, lb(S1, 0, (uint16_t) DEBUG_RAM_START + 24));
            gs.write_debug_ram(2, sb(S1, S0, 0));
            gs.write_debug_ram(6, data[0]);
            break;
          case 2:
            gs.write_debug_ram(1, lh(S1, 0, (uint16_t) DEBUG_RAM_START + 24));
            gs.write_debug_ram(2, sh(S1, S0, 0));
            gs.write_debug_ram(6, data[0] | (data[1] << 8));
            break;
          case 4:
            gs.write_debug_ram(1, lw(S1, 0, (uint16_t) DEBUG_RAM_START + 24));
            gs.write_debug_ram(2, sw(S1, S0, 0));
            gs.write_debug_ram(6, data[0] | (data[1] << 8) |
                (data[2] << 16) | (data[3] << 24));
            break;
          case 8:
            gs.write_debug_ram(1, ld(S1, 0, (uint16_t) DEBUG_RAM_START + 24));
            gs.write_debug_ram(2, sd(S1, S0, 0));
            gs.write_debug_ram(6, data[0] | (data[1] << 8) |
                (data[2] << 16) | (data[3] << 24));
            gs.write_debug_ram(7, data[4] | (data[5] << 8) |
                (data[6] << 16) | (data[7] << 24));
            break;
          default:
            gs.send_packet("E12");
            return true;
        }
        gs.write_debug_ram(3, jal(0, (uint32_t) (DEBUG_ROM_RESUME - (DEBUG_RAM_START + 4*3))));
        gs.write_debug_ram(4, addr);
        gs.write_debug_ram(5, addr >> 32);
        gs.set_interrupt(0);

        return false;
      }

      offset += access_size;
      if (offset >= length) {
        gs.send_packet("OK");
        return true;
      } else {
      const unsigned char *d = data + offset;
      switch (access_size) {
        case 1:
          gs.write_debug_ram(6, d[0]);
          break;
        case 2:
          gs.write_debug_ram(6, d[0] | (d[1] << 8));
          break;
        case 4:
          gs.write_debug_ram(6, d[0] | (d[1] << 8) |
              (d[2] << 16) | (d[3] << 24));
          break;
        case 8:
          gs.write_debug_ram(6, d[0] | (d[1] << 8) |
              (d[2] << 16) | (d[3] << 24));
          gs.write_debug_ram(7, d[4] | (d[5] << 8) |
              (d[6] << 16) | (d[7] << 24));
          break;
        default:
          gs.send_packet("E12");
          return true;
      }
        gs.write_debug_ram(4, addr + offset);
        gs.write_debug_ram(5, (addr + offset) >> 32);
        gs.set_interrupt(0);
        return false;
      }
    }

  private:
    reg_t addr;
    unsigned int offset;
    unsigned int length;
    unsigned int access_size;
    unsigned char *data;
};

class collect_translation_info_op_t : public operation_t
{
  public:
    // Read sufficient information from the target into gdbserver structures so
    // that it's possible to translate vaddr, vaddr+length, and all addresses
    // in between to physical addresses.
    collect_translation_info_op_t(gdbserver_t& gdbserver, reg_t vaddr, size_t length) :
      operation_t(gdbserver), vaddr(vaddr), length(length) {};

    bool perform_step(unsigned int step)
    {
      unsigned int vm = get_field(gs.saved_mstatus, MSTATUS_VM);

      if (step == 0) {
        switch (vm) {
          case VM_MBARE:
            // Nothing to be done.
            return true;

          default:
            {
              char buf[100];
              sprintf(buf, "VM mode %d is not supported by gdbserver.cc.", vm);
              die(buf);
              return true;        // die doesn't return, but gcc doesn't know that.
            }
        }
      }
      return true;
    }

  private:
    reg_t vaddr;
    size_t length;
};

////////////////////////////// gdbserver itself

gdbserver_t::gdbserver_t(uint16_t port, sim_t *sim) :
  sim(sim),
  client_fd(0),
  recv_buf(64 * 1024), send_buf(64 * 1024)
{
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    fprintf(stderr, "failed to make socket: %s (%d)\n", strerror(errno), errno);
    abort();
  }

  fcntl(socket_fd, F_SETFL, O_NONBLOCK);
  int reuseaddr = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
        sizeof(int)) == -1) {
    fprintf(stderr, "failed setsockopt: %s (%d)\n", strerror(errno), errno);
    abort();
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(socket_fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
    fprintf(stderr, "failed to bind socket: %s (%d)\n", strerror(errno), errno);
    abort();
  }

  if (listen(socket_fd, 1) == -1) {
    fprintf(stderr, "failed to listen on socket: %s (%d)\n", strerror(errno), errno);
    abort();
  }
}

void gdbserver_t::write_debug_ram(unsigned int index, uint32_t value)
{
  sim->debug_module.ram_write32(index, value);
}

uint32_t gdbserver_t::read_debug_ram(unsigned int index)
{
  return sim->debug_module.ram_read32(index);
}

void gdbserver_t::add_operation(operation_t* operation)
{
  operation_queue.push(operation);
}

void gdbserver_t::accept()
{
  client_fd = ::accept(socket_fd, NULL, NULL);
  if (client_fd == -1) {
    if (errno == EAGAIN) {
      // No client waiting to connect right now.
    } else {
      fprintf(stderr, "failed to accept on socket: %s (%d)\n", strerror(errno),
          errno);
      abort();
    }
  } else {
    fcntl(client_fd, F_SETFL, O_NONBLOCK);

    expect_ack = false;
    extended_mode = false;

    // gdb wants the core to be halted when it attaches.
    add_operation(new halt_op_t(*this));
  }
}

void gdbserver_t::read()
{
  // Reading from a non-blocking socket still blocks if there is no data
  // available.

  size_t count = recv_buf.contiguous_empty_size();
  assert(count > 0);
  ssize_t bytes = ::read(client_fd, recv_buf.contiguous_empty(), count);
  if (bytes == -1) {
    if (errno == EAGAIN) {
      // We'll try again the next call.
    } else {
      fprintf(stderr, "failed to read on socket: %s (%d)\n", strerror(errno), errno);
      abort();
    }
  } else if (bytes == 0) {
    // The remote disconnected.
    client_fd = 0;
    processor_t *p = sim->get_core(0);
    // TODO p->set_halted(false, HR_NONE);
    recv_buf.reset();
    send_buf.reset();
  } else {
    recv_buf.data_added(bytes);
  }
}

void gdbserver_t::write()
{
  if (send_buf.empty())
    return;

  while (!send_buf.empty()) {
    unsigned int count = send_buf.contiguous_data_size();
    assert(count > 0);
    ssize_t bytes = ::write(client_fd, send_buf.contiguous_data(), count);
    if (bytes == -1) {
      fprintf(stderr, "failed to write to socket: %s (%d)\n", strerror(errno), errno);
      abort();
    } else if (bytes == 0) {
      // Client can't take any more data right now.
      break;
    } else {
      fprintf(stderr, "wrote %ld bytes: ", bytes);
      for (unsigned int i = 0; i < bytes; i++) {
        fprintf(stderr, "%c", send_buf[i]);
      }
      fprintf(stderr, "\n");
      send_buf.consume(bytes);
    }
  }
}

void print_packet(const std::vector<uint8_t> &packet)
{
  for (uint8_t c : packet) {
    if (c >= ' ' and c <= '~')
      fprintf(stderr, "%c", c);
    else
      fprintf(stderr, "\\x%x", c);
  }
  fprintf(stderr, "\n");
}

uint8_t compute_checksum(const std::vector<uint8_t> &packet)
{
  uint8_t checksum = 0;
  for (auto i = packet.begin() + 1; i != packet.end() - 3; i++ ) {
    checksum += *i;
  }
  return checksum;
}

uint8_t character_hex_value(uint8_t character)
{
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return 10 + character - 'a';
  if (character >= 'A' && character <= 'F')
    return 10 + character - 'A';
  return 0xff;
}

uint8_t extract_checksum(const std::vector<uint8_t> &packet)
{
  return character_hex_value(*(packet.end() - 1)) +
    16 * character_hex_value(*(packet.end() - 2));
}

void gdbserver_t::process_requests()
{
  // See https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html

  while (!recv_buf.empty()) {
    std::vector<uint8_t> packet;
    for (unsigned int i = 0; i < recv_buf.size(); i++) {
      uint8_t b = recv_buf[i];

      if (packet.empty() && expect_ack && b == '+') {
        recv_buf.consume(1);
        break;
      }

      if (packet.empty() && b == 3) {
        fprintf(stderr, "Received interrupt\n");
        recv_buf.consume(1);
        handle_interrupt();
        break;
      }

      if (b == '$') {
        // Start of new packet.
        if (!packet.empty()) {
          fprintf(stderr, "Received malformed %ld-byte packet from debug client: ",
              packet.size());
          print_packet(packet);
          recv_buf.consume(i);
          break;
        }
      }

      packet.push_back(b);

      // Packets consist of $<packet-data>#<checksum>
      // where <checksum> is 
      if (packet.size() >= 4 &&
          packet[packet.size()-3] == '#') {
        handle_packet(packet);
        recv_buf.consume(i+1);
        break;
      }
    }
    // There's a partial packet in the buffer. Wait until we get more data to
    // process it.
    if (packet.size()) {
      break;
    }
  }
}

void gdbserver_t::handle_halt_reason(const std::vector<uint8_t> &packet)
{
  send_packet("S00");
}

void gdbserver_t::handle_general_registers_read(const std::vector<uint8_t> &packet)
{
  add_operation(new general_registers_read_op_t(*this));
}

void gdbserver_t::set_interrupt(uint32_t hartid) {
  sim->debug_module.set_interrupt(hartid);
}

// First byte is the most-significant one.
// Eg. "08675309" becomes 0x08675309.
uint64_t consume_hex_number(std::vector<uint8_t>::const_iterator &iter,
    std::vector<uint8_t>::const_iterator end)
{
  uint64_t value = 0;

  while (iter != end) {
    uint8_t c = *iter;
    uint64_t c_value = character_hex_value(c);
    if (c_value > 15)
      break;
    iter++;
    value <<= 4;
    value += c_value;
  }
  return value;
}

// First byte is the least-significant one.
// Eg. "08675309" becomes 0x09536708
uint64_t consume_hex_number_le(std::vector<uint8_t>::const_iterator &iter,
    std::vector<uint8_t>::const_iterator end)
{
  uint64_t value = 0;
  unsigned int shift = 4;

  while (iter != end) {
    uint8_t c = *iter;
    uint64_t c_value = character_hex_value(c);
    if (c_value > 15)
      break;
    iter++;
    value |= c_value << shift;
    if ((shift % 8) == 0)
      shift += 12;
    else
      shift -= 4;
  }
  return value;
}

void consume_string(std::string &str, std::vector<uint8_t>::const_iterator &iter,
    std::vector<uint8_t>::const_iterator end, uint8_t separator)
{
  while (iter != end && *iter != separator) {
    str.append(1, (char) *iter);
    iter++;
  }
}

void gdbserver_t::handle_register_read(const std::vector<uint8_t> &packet)
{
  // p n

  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
  unsigned int n = consume_hex_number(iter, packet.end());
  if (*iter != '#')
    return send_packet("E01");

  add_operation(new register_read_op_t(*this, n));
}

void gdbserver_t::handle_register_write(const std::vector<uint8_t> &packet)
{
  // P n...=r...

  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
  unsigned int n = consume_hex_number(iter, packet.end());
  if (*iter != '=')
    return send_packet("E05");
  iter++;

  reg_t value = consume_hex_number_le(iter, packet.end());
  if (*iter != '#')
    return send_packet("E06");

  processor_t *p = sim->get_core(0);

  die("handle_register_write");
  /*
  if (n >= REG_XPR0 && n <= REG_XPR31) {
    p->state.XPR.write(n - REG_XPR0, value);
  } else if (n == REG_PC) {
    p->state.pc = value;
  } else if (n >= REG_FPR0 && n <= REG_FPR31) {
    p->state.FPR.write(n - REG_FPR0, value);
  } else if (n >= REG_CSR0 && n <= REG_CSR4095) {
    try {
      p->set_csr(n - REG_CSR0, value);
    } catch(trap_t& t) {
      return send_packet("EFF");
    }
  } else {
    return send_packet("E07");
  }
  */

  return send_packet("OK");
}

void gdbserver_t::handle_memory_read(const std::vector<uint8_t> &packet)
{
  // m addr,length
  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
  reg_t address = consume_hex_number(iter, packet.end());
  if (*iter != ',')
    return send_packet("E10");
  iter++;
  reg_t length = consume_hex_number(iter, packet.end());
  if (*iter != '#')
    return send_packet("E11");

  add_operation(new memory_read_op_t(*this, address, length));
}

void gdbserver_t::handle_memory_binary_write(const std::vector<uint8_t> &packet)
{
  // X addr,length:XX...
  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
  reg_t address = consume_hex_number(iter, packet.end());
  if (*iter != ',')
    return send_packet("E20");
  iter++;
  reg_t length = consume_hex_number(iter, packet.end());
  if (*iter != ':')
    return send_packet("E21");
  iter++;

  if (length == 0) {
    return send_packet("OK");
  }

  unsigned char *data = new unsigned char[length];
  for (unsigned int i = 0; i < length; i++) {
    if (iter == packet.end()) {
      return send_packet("E22");
    }
    data[i] = *iter;
    iter++;
  }
  if (*iter != '#')
    return send_packet("E4b"); // EOVERFLOW

  add_operation(new memory_write_op_t(*this, address, length, data));
}

void gdbserver_t::handle_continue(const std::vector<uint8_t> &packet)
{
  // c [addr]
  processor_t *p = sim->get_core(0);
  if (packet[2] != '#') {
    std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
    saved_dpc = consume_hex_number(iter, packet.end());
    if (*iter != '#')
      return send_packet("E30");
  }

  add_operation(new continue_op_t(*this));
}

void gdbserver_t::handle_step(const std::vector<uint8_t> &packet)
{
  // s [addr]
  if (packet[2] != '#') {
    std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
    die("handle_step");
    //p->state.pc = consume_hex_number(iter, packet.end());
    if (*iter != '#')
      return send_packet("E40");
  }

  // TODO: p->set_single_step(true);
  // TODO running = true;
}

void gdbserver_t::handle_kill(const std::vector<uint8_t> &packet)
{
  // k
  // The exact effect of this packet is not specified.
  // Looks like OpenOCD disconnects?
  // TODO
}

void gdbserver_t::handle_extended(const std::vector<uint8_t> &packet)
{
  // Enable extended mode. In extended mode, the remote server is made
  // persistent. The ‘R’ packet is used to restart the program being debugged.
  send_packet("OK");
  extended_mode = true;
}

void software_breakpoint_t::insert(mmu_t* mmu)
{
  if (size == 2) {
    instruction = mmu->load_uint16(address);
    mmu->store_uint16(address, C_EBREAK);
  } else {
    instruction = mmu->load_uint32(address);
    mmu->store_uint32(address, EBREAK);
  }
  fprintf(stderr, ">>> Read %x from %lx\n", instruction, address);
}

void software_breakpoint_t::remove(mmu_t* mmu)
{
  fprintf(stderr, ">>> write %x to %lx\n", instruction, address);
  if (size == 2) {
    mmu->store_uint16(address, instruction);
  } else {
    mmu->store_uint32(address, instruction);
  }
}

void gdbserver_t::handle_breakpoint(const std::vector<uint8_t> &packet)
{
  // insert: Z type,addr,kind
  // remove: z type,addr,kind

  software_breakpoint_t bp;
  bool insert = (packet[1] == 'Z');
  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;
  int type = consume_hex_number(iter, packet.end());
  if (*iter != ',')
    return send_packet("E50");
  iter++;
  bp.address = consume_hex_number(iter, packet.end());
  if (*iter != ',')
    return send_packet("E51");
  iter++;
  bp.size = consume_hex_number(iter, packet.end());
  // There may be more options after a ; here, but we don't support that.
  if (*iter != '#')
    return send_packet("E52");

  if (bp.size != 2 && bp.size != 4) {
    return send_packet("E53");
  }

  processor_t *p = sim->get_core(0);
  die("handle_breakpoint");
  /*
  mmu_t* mmu = p->mmu;
  if (insert) {
    bp.insert(mmu);
    breakpoints[bp.address] = bp;

  } else {
    bp = breakpoints[bp.address];
    bp.remove(mmu);
    breakpoints.erase(bp.address);
  }
  mmu->flush_icache();
  sim->debug_mmu->flush_icache();
  */
  return send_packet("OK");
}

void gdbserver_t::handle_query(const std::vector<uint8_t> &packet)
{
  std::string name;
  std::vector<uint8_t>::const_iterator iter = packet.begin() + 2;

  consume_string(name, iter, packet.end(), ':');
  if (iter != packet.end())
    iter++;
  if (name == "Supported") {
    start_packet();
    while (iter != packet.end()) {
      std::string feature;
      consume_string(feature, iter, packet.end(), ';');
      if (iter != packet.end())
        iter++;
      if (feature == "swbreak+") {
        send("swbreak+;");
      }
    }
    return end_packet();
  }

  fprintf(stderr, "Unsupported query %s\n", name.c_str());
  return send_packet("");
}

void gdbserver_t::handle_packet(const std::vector<uint8_t> &packet)
{
  if (compute_checksum(packet) != extract_checksum(packet)) {
    fprintf(stderr, "Received %ld-byte packet with invalid checksum\n", packet.size());
    fprintf(stderr, "Computed checksum: %x\n", compute_checksum(packet));
    print_packet(packet);
    send("-");
    return;
  }

  fprintf(stderr, "Received %ld-byte packet from debug client: ", packet.size());
  print_packet(packet);
  send("+");

  switch (packet[1]) {
    case '!':
      return handle_extended(packet);
    case '?':
      return handle_halt_reason(packet);
    case 'g':
      return handle_general_registers_read(packet);
    case 'k':
      return handle_kill(packet);
    case 'm':
      return handle_memory_read(packet);
//    case 'M':
//      return handle_memory_write(packet);
    case 'X':
      return handle_memory_binary_write(packet);
    case 'p':
      return handle_register_read(packet);
    case 'P':
      return handle_register_write(packet);
    case 'c':
      return handle_continue(packet);
    case 's':
      return handle_step(packet);
    case 'z':
    case 'Z':
      return handle_breakpoint(packet);
    case 'q':
    case 'Q':
      return handle_query(packet);
  }

  // Not supported.
  fprintf(stderr, "** Unsupported packet: ");
  print_packet(packet);
  send_packet("");
}

void gdbserver_t::handle_interrupt()
{
  processor_t *p = sim->get_core(0);
  // TODO p->set_halted(true, HR_INTERRUPT);
  send_packet("S02");   // Pretend program received SIGINT.
  // TODO running = false;
}

void gdbserver_t::handle()
{
  if (client_fd > 0) {
    processor_t *p = sim->get_core(0);

    bool interrupt = sim->debug_module.get_interrupt(0);

    if (!interrupt && !operation_queue.empty()) {
      operation_t *operation = operation_queue.front();
      if (operation->step()) {
        operation_queue.pop();
        delete operation;
      }
    }

    /* TODO
    if (running && p->halted) {
      // The core was running, but now it's halted. Better tell gdb.
      switch (p->halt_reason) {
        case HR_NONE:
          fprintf(stderr, "Internal error. Processor halted without reason.\n");
          abort();
        case HR_STEPPED:
        case HR_INTERRUPT:
        case HR_CMDLINE:
        case HR_ATTACHED:
          // There's no gdb code for this.
          send_packet("T05");
          break;
        case HR_SWBP:
          send_packet("T05swbreak:;");
          break;
      }
      send_packet("T00");
      // TODO: Actually include register values here
      running = false;
    }
      */

    this->read();
    this->write();

  } else {
    this->accept();
  }

  if (operation_queue.empty()) {
    this->process_requests();
  }
}

void gdbserver_t::send(const char* msg)
{
  unsigned int length = strlen(msg);
  for (const char *c = msg; *c; c++)
    running_checksum += *c;
  send_buf.append((const uint8_t *) msg, length);
}

void gdbserver_t::send(uint64_t value)
{
  char buffer[3];
  for (unsigned int i = 0; i < 8; i++) {
    sprintf(buffer, "%02x", (int) (value & 0xff));
    send(buffer);
    value >>= 8;
  }
}

void gdbserver_t::send(uint32_t value)
{
  char buffer[3];
  for (unsigned int i = 0; i < 4; i++) {
    sprintf(buffer, "%02x", (int) (value & 0xff));
    send(buffer);
    value >>= 8;
  }
}

void gdbserver_t::send_packet(const char* data)
{
  start_packet();
  send(data);
  end_packet();
  expect_ack = true;
}

void gdbserver_t::start_packet()
{
  send("$");
  running_checksum = 0;
}

void gdbserver_t::end_packet(const char* data)
{
  if (data) {
    send(data);
  }

  char checksum_string[4];
  sprintf(checksum_string, "#%02x", running_checksum);
  send(checksum_string);
  expect_ack = true;
}
