#include "x86_64-qemu.h"
#include <stdarg.h>

#define IRQHANDLE_DECL(id, dpl, err)  void __am_irq##id();
IRQS(IRQHANDLE_DECL)
void __am_irqall();
static _Context* (*user_handler)(_Event, _Context*) = NULL;

static void __am_irq_handle_internal(struct trap_frame *tf) {
  _Context saved_ctx;
  _Event ev = {
    .event = _EVENT_NULL,
    .cause = 0, .ref = 0,
    .msg = "(no message)",
  };
 
  printf("[%d/%d] ", tf->irq, _cpu());

#define dump(name) printf("%s = %08x (%d)\n", #name, saved_ctx.name, saved_ctx.name);
#define offset(name) printf("%s @ %d\n", #name, (char *)&tf.name - (char*)tf);
#if __x86_64
  saved_ctx        = tf->saved_context;
  saved_ctx.rip    = tf->rip;
  saved_ctx.cs     = tf->cs;
  saved_ctx.rflags = tf->rflags;
  saved_ctx.rsp    = tf->rsp;
  saved_ctx.ss     = tf->ss;

  /*
  dump(rax); dump(rbx); dump(rcx); dump(rdx); dump(rbp); dump(rsp); dump(rsi); dump(rdi);
  dump(r8); dump(r9); dump(r10); dump(r11); dump(r12); dump(r13); dump(r14); dump(r15);
  dump(cs); dump(ss); dump(rip); dump(rflags);
  */
#else
  saved_ctx     = tf->saved_context;
  saved_ctx.eip = tf->eip;
  saved_ctx.cs  = tf->cs;
  saved_ctx.eflags = tf->eflags;
  if (tf->cs & DPL_USER) {
  } else {
    saved_ctx.esp = (uint32_t)(tf + 1) - 8; // no ss/esp saved
    saved_ctx.ss  = 0;
  }

/*
  dump(eax); dump(ebx); dump(ecx); dump(edx); 
  dump(ebp); dump(esp); dump(esi); dump(edi);
  dump(cs); dump(ds); dump(eip); dump(eflags); 
*/
#endif

  #define IRQ    T_IRQ0 + 
  #define MSG(m) ev.msg = m;

  if (IRQ 0 <= tf->irq && tf->irq < IRQ 32) {
    __am_lapic_eoi();
  }

  switch (tf->irq) {
    case IRQ 0: MSG("timer interrupt (lapic)")
      ev.event = _EVENT_IRQ_TIMER; break;
    case IRQ 1: MSG("I/O device IRQ1 (keyboard)")
      ev.event = _EVENT_IRQ_IODEV; break;
    case EX_SYSCALL: MSG("int $0x80 trap: _yield() or system call")
      if ((int32_t)saved_ctx.GPR1 == -1) {
        ev.event = _EVENT_YIELD;
      } else {
        ev.event = _EVENT_SYSCALL;
      }
      break;
    case EX_DIV: MSG("divide by zero")
      ev.event = _EVENT_ERROR; break;
    case EX_UD: MSG("UD #6 invalid opcode")
      ev.event = _EVENT_ERROR; break;
    case EX_NM: MSG("NM #7 coprocessor error")
      ev.event = _EVENT_ERROR; break;
    case EX_DF: MSG("DF #8 double fault")
      ev.event = _EVENT_ERROR; break;
    case EX_TS: MSG("TS #10 invalid TSS")
      ev.event = _EVENT_ERROR; break;
    case EX_NP: MSG("NP #11 segment/gate not present")
      ev.event = _EVENT_ERROR; break;
    case EX_SS: MSG("SS #12 stack fault")
      ev.event = _EVENT_ERROR; break;
    case EX_GP: MSG("GP #13, general protection fault")
      ev.event = _EVENT_ERROR; break;
    case EX_PF: MSG("PF #14, page fault, @cause: _PROT_XXX")
      ev.event = _EVENT_PAGEFAULT;
      if (tf->errcode & 0x1) ev.cause |= _PROT_NONE;
      if (tf->errcode & 0x2) ev.cause |= _PROT_WRITE;
      else                   ev.cause |= _PROT_READ;
      ev.ref = get_cr2();
      break;
    default: MSG("unrecognized interrupt/exception")
      ev.event = _EVENT_ERROR;
      ev.cause = tf->errcode;
      break;
  }

  _Context *ret_ctx = user_handler(ev, &saved_ctx);
  __am_iret(ret_ctx ? ret_ctx : &saved_ctx);
}

void __am_irq_handle(struct trap_frame *tf) {
  stack_switch_call(stack_top(&CPU->irq_stack), __am_irq_handle_internal, (uintptr_t)tf);
}

#if __x86_64__
static GateDesc64 idt[NR_IRQ];
#define GATE GATE64
#else
static GateDesc32 idt[NR_IRQ];
#define GATE GATE32
#endif


int _cte_init(_Context *(*handler)(_Event, _Context *)) {
  if (_cpu() != 0) panic("init CTE in non-bootstrap CPU");
  if (!handler) panic("no interrupt handler");

  for (int i = 0; i < NR_IRQ; i ++) {
    idt[i] = GATE(STS_TG, KSEL(SEG_KCODE), __am_irqall, DPL_KERN);
  }
#define IDT_ENTRY(id, dpl, err) \
  idt[id] = GATE(STS_TG, KSEL(SEG_KCODE), __am_irq##id, DPL_##dpl);
  IRQS(IDT_ENTRY)

  user_handler = handler;
  return 0;
}

void _yield() {
  asm volatile ("int $0x80" : : "a"(-1));
}

int _intr_read() {
  return (get_efl() & FL_IF) != 0;
}

void _intr_write(int enable) {
  if (enable) {
    sti();
  } else {
    cli();
  }
}

static void panic_on_return() { panic("kernel context returns"); }

_Context *_kcontext(_Area stack, void (*entry)(void *), void *arg) {
   _Area stk_aligned = {
    (void *)ROUNDUP(stack.start, 16),
    (void *)ROUNDDOWN(stack.end, 16),
  };
  uintptr_t stk_top = (uintptr_t)stk_aligned.end;

  _Context *ctx = (_Context *)stk_aligned.start;
  *ctx = (_Context) { 0 };

#if __x86_64__
#define sp rsp
  ctx->rsp = stk_top;
  ctx->cs = KSEL(SEG_KCODE);
  ctx->rip = (uintptr_t)entry;
  ctx->rflags = FL_IF;
  void *values[] = { panic_on_return }; 
#else
#define sp esp
  ctx->ds = ctx->ss = KSEL(SEG_KDATA);
  ctx->cs = KSEL(SEG_KCODE);
  ctx->eip = (uint32_t)entry;
  ctx->esp = stk_top;
  ctx->eflags = FL_IF;

  void *values[] = { panic_on_return, arg }; 
#endif
  ctx->sp -= sizeof(values);
  for (int i = 0; i < LENGTH(values); i++) {
    ((uintptr_t *)ctx->sp)[i] = (uintptr_t)values[i];
  }
  return ctx;
}

void __am_percpu_initirq() {
  __am_ioapic_enable(IRQ_KBD, 0);
  set_idt(idt, sizeof(idt));
}
