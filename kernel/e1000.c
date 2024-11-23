#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;
struct spinlock e1000_tx_lock;
struct spinlock e1000_rx_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");
  initlock(&e1000_tx_lock, "e1000_tx_lock");
  initlock(&e1000_rx_lock, "e1000_rx_lock");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_bufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();
    if (!rx_bufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_bufs[i];
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
next_e1000_transmit_index(int index)
{
  return (index + 1) % TX_RING_SIZE;
}

int
e1000_transmit(char *buf, int len)
{
  //
  // Your code here.
  //
  // buf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after send completes.
  //
  acquire(&e1000_tx_lock);

  int tail_index = regs[E1000_TDT];
  int head_index = regs[E1000_TDH];

  if(next_e1000_transmit_index(tail_index) == head_index)
    goto transmit_err;

  if((tx_ring[tail_index].status & E1000_TXD_STAT_DD) == 0)
    goto transmit_err;

  if(tx_bufs[tail_index] != 0)
    kfree(tx_bufs[tail_index]);

  tx_bufs[tail_index] = buf;

  tx_ring[tail_index].addr = (uint64) buf;
  tx_ring[tail_index].length = len;
  tx_ring[tail_index].cso = 0;
  tx_ring[tail_index].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  tx_ring[tail_index].status = 0;
  tx_ring[tail_index].css = 0;
  tx_ring[tail_index].special = 0;

  regs[E1000_TDT] = next_e1000_transmit_index(tail_index);

  release(&e1000_tx_lock);
  return 0;

transmit_err:
  release(&e1000_tx_lock);
  kfree(buf);
  return -1;
}

int
next_e1000_recv_index(int index)
{
  return (index + 1) % RX_RING_SIZE;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver a buf for each packet (using net_rx()).
  //
  acquire(&e1000_rx_lock);

  while(1){
    int next_index = next_e1000_recv_index(regs[E1000_RDT]);

    if((rx_ring[next_index].status & E1000_RXD_STAT_DD) == 0)
      break;

    net_rx(rx_bufs[next_index], rx_ring[next_index].length);

    rx_bufs[next_index] = kalloc();
    if (!rx_bufs[next_index])
      panic("e1000_recv");
    rx_ring[next_index].addr = (uint64) rx_bufs[next_index];
    rx_ring[next_index].length = 0;
    rx_ring[next_index].csum = 0;
    rx_ring[next_index].status = E1000_RXD_STAT_EOP;
    rx_ring[next_index].errors = 0;
    rx_ring[next_index].special = 0;

    regs[E1000_RDT] = next_index;
  }

  release(&e1000_rx_lock);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
