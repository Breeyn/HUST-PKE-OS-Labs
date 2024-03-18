/*
 * virtual address mapping related functions.
 */

#include "vmm.h"
#include "riscv.h"
#include "pmm.h"
#include "util/types.h"
#include "memlayout.h"
#include "util/string.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"
#include "process.h"

struct MCB_t *MCB_head = NULL;  //MCB链表头
uint64 pt_top_va=USER_FREE_ADDRESS_START; //页表的顶部地址
uint64 first_malloc = 1;  // 首次分配

/* --- utility functions for virtual address mapping --- */
//
// establish mapping of virtual address [va, va+size] to phyiscal address [pa, pa+size]
// with the permission of "perm".
//
int map_pages(pagetable_t page_dir, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 first, last;
  pte_t *pte;

  for (first = ROUNDDOWN(va, PGSIZE), last = ROUNDDOWN(va + size - 1, PGSIZE);
      first <= last; first += PGSIZE, pa += PGSIZE) {
    if ((pte = page_walk(page_dir, first, 1)) == 0) return -1;
    if (*pte & PTE_V)
      panic("map_pages fails on mapping va (0x%lx) to pa (0x%lx)", first, pa);
    *pte = PA2PTE(pa) | perm | PTE_V;
  }
  return 0;
}

//
// convert permission code to permission types of PTE
//
uint64 prot_to_type(int prot, int user) {
  uint64 perm = 0;
  if (prot & PROT_READ) perm |= PTE_R | PTE_A;
  if (prot & PROT_WRITE) perm |= PTE_W | PTE_D;
  if (prot & PROT_EXEC) perm |= PTE_X | PTE_A;
  if (perm == 0) perm = PTE_R;
  if (user) perm |= PTE_U;
  return perm;
}

//
// traverse the page table (starting from page_dir) to find the corresponding pte of va.
// returns: PTE (page table entry) pointing to va.
//
pte_t *page_walk(pagetable_t page_dir, uint64 va, int alloc) {
  if (va >= MAXVA) panic("page_walk");

  // starting from the page directory
  pagetable_t pt = page_dir;

  // traverse from page directory to page table.
  // as we use risc-v sv39 paging scheme, there will be 3 layers: page dir,
  // page medium dir, and page table.
  for (int level = 2; level > 0; level--) {
    // macro "PX" gets the PTE index in page table of current level
    // "pte" points to the entry of current level
    pte_t *pte = pt + PX(level, va);

    // now, we need to know if above pte is valid (established mapping to a phyiscal page)
    // or not.
    if (*pte & PTE_V) {  //PTE valid
      // phisical address of pagetable of next level
      pt = (pagetable_t)PTE2PA(*pte);
    } else { //PTE invalid (not exist).
      // allocate a page (to be the new pagetable), if alloc == 1
      if( alloc && ((pt = (pte_t *)alloc_page(1)) != 0) ){
        memset(pt, 0, PGSIZE);
        // writes the physical address of newly allocated page to pte, to establish the
        // page table tree.
        *pte = PA2PTE(pt) | PTE_V;
      }else //returns NULL, if alloc == 0, or no more physical page remains
        return 0;
    }
  }

  // return a PTE which contains phisical address of a page
  return pt + PX(0, va);
}

//
// look up a virtual page address, return the physical page address or 0 if not mapped.
//
uint64 lookup_pa(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = page_walk(pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_V) == 0 || ((*pte & PTE_R) == 0 && (*pte & PTE_W) == 0))
    return 0;
  pa = PTE2PA(*pte);

  return pa;
}

/* --- kernel page table part --- */
// _etext is defined in kernel.lds, it points to the address after text and rodata segments.
extern char _etext[];

// pointer to kernel page director
pagetable_t g_kernel_pagetable;

//
// maps virtual address [va, va+sz] to [pa, pa+sz] (for kernel).
//
void kern_vm_map(pagetable_t page_dir, uint64 va, uint64 pa, uint64 sz, int perm) {
  // map_pages is defined in kernel/vmm.c
  if (map_pages(page_dir, va, sz, pa, perm) != 0) panic("kern_vm_map");
}

//
// kern_vm_init() constructs the kernel page table.
//
void kern_vm_init(void) {
  // pagetable_t is defined in kernel/riscv.h. it's actually uint64*
  pagetable_t t_page_dir;

  // allocate a page (t_page_dir) to be the page directory for kernel. alloc_page is defined in kernel/pmm.c
  t_page_dir = (pagetable_t)alloc_page();
  // memset is defined in util/string.c
  memset(t_page_dir, 0, PGSIZE);

  // map virtual address [KERN_BASE, _etext] to physical address [DRAM_BASE, DRAM_BASE+(_etext - KERN_BASE)],
  // to maintain (direct) text section kernel address mapping.
  kern_vm_map(t_page_dir, KERN_BASE, DRAM_BASE, (uint64)_etext - KERN_BASE,
         prot_to_type(PROT_READ | PROT_EXEC, 0));

  sprint("KERN_BASE 0x%lx\n", lookup_pa(t_page_dir, KERN_BASE));

  // also (direct) map remaining address space, to make them accessable from kernel.
  // this is important when kernel needs to access the memory content of user's app
  // without copying pages between kernel and user spaces.
  kern_vm_map(t_page_dir, (uint64)_etext, (uint64)_etext, PHYS_TOP - (uint64)_etext,
         prot_to_type(PROT_READ | PROT_WRITE, 0));

  sprint("physical address of _etext is: 0x%lx\n", lookup_pa(t_page_dir, (uint64)_etext));

  g_kernel_pagetable = t_page_dir;
}

/* --- user page table part --- */
//
// convert and return the corresponding physical address of a virtual address (va) of
// application.
//
void *user_va_to_pa(pagetable_t page_dir, void *va) {
  // TODO (lab2_1): implement user_va_to_pa to convert a given user virtual address "va"
  // to its corresponding physical address, i.e., "pa". To do it, we need to walk
  // through the page table, starting from its directory "page_dir", to locate the PTE
  // that maps "va". If found, returns the "pa" by using:
  // pa = PYHS_ADDR(PTE) + (va & (1<<PGSHIFT -1))
  // Here, PYHS_ADDR() means retrieving the starting address (4KB aligned), and
  // (va & (1<<PGSHIFT -1)) means computing the offset of "va" inside its page.
  // Also, it is possible that "va" is not mapped at all. in such case, we can find
  // invalid PTE, and should return NULL.
  //panic( "You have to implement user_va_to_pa (convert user va to pa) to print messages in lab2_1.\n" );
  uint64 physical_address = lookup_pa(page_dir, ((uint64)va)) + ((unsigned long long)va & ((1<<PGSHIFT)-1));
  return (void*)physical_address;
}

//
// maps virtual address [va, va+sz] to [pa, pa+sz] (for user application).
//
void user_vm_map(pagetable_t page_dir, uint64 va, uint64 size, uint64 pa, int perm) {
  if (map_pages(page_dir, va, size, pa, perm) != 0) {
    panic("fail to user_vm_map .\n");
  }
}

//
// unmap virtual address [va, va+size] from the user app.
// reclaim the physical pages if free!=0
//
void user_vm_unmap(pagetable_t page_dir, uint64 va, uint64 size, int free) {
  // TODO (lab2_2): implement user_vm_unmap to disable the mapping of the virtual pages
  // in [va, va+size], and free the corresponding physical pages used by the virtual
  // addresses when if 'free' (the last parameter) is not zero.
  // basic idea here is to first locate the PTEs of the virtual pages, and then reclaim
  // (use free_page() defined in pmm.c) the physical pages. lastly, invalidate the PTEs.
  // as naive_free reclaims only one page at a time, you only need to consider one page
  // to make user/app_naive_malloc to behave correctly.
  // panic( "You have to implement user_vm_unmap to free pages using naive_free in lab2_2.\n" );

  pte_t* pte;
  pte=page_walk(page_dir,va,0);
  free_page((void*)(((*pte)>>10)<<12));
  *pte= *pte & (~PTE_V);

}

void init_MCBs(){
  if (first_malloc){
    uint64 va_=pt_top_va;
    malloc_mapping(sizeof(MCB));
    pte_t *pte = page_walk(current->pagetable,USER_FREE_ADDRESS_START,0);
    MCB_head=(MCB*)PTE2PA(*pte);
    MCB_head->m_state = 0;
    MCB_head->m_startaddr = *pte + (sizeof(MCB));
    MCB_head->m_size = 0;
    MCB_head->next=NULL;
    first_malloc=0;
  }
}

void malloc_mapping(uint64 n){
  uint64 tmp=ROUNDUP(pt_top_va,PGSIZE);
  for (uint64 i=tmp;i<pt_top_va+n;i+=PGSIZE){
    char* new_mem_alloc = (char*)alloc_page();
    memset(new_mem_alloc,0,PGSIZE);
    map_pages(current->pagetable,tmp,PGSIZE,(uint64)new_mem_alloc,prot_to_type(PROT_READ | PROT_WRITE,1));
  }
  pt_top_va+=n;//进入下一段
  return;
}

uint64 user_better_malloc(uint64 n){
  init_MCBs();
  //sprint("%d\n", sizeof(MCB_head->next));
  MCB* tmp = (MCB*)MCB_head;
  for(;;){
    // 查找合适的块
    if (tmp->m_size>=n && tmp->m_state == 0){
      tmp->m_state=1;
      return tmp->m_startaddr;
    }
    if (!(tmp->next)) break;
    tmp=tmp->next;
  }
  uint64 top_heap = pt_top_va;//堆顶
  malloc_mapping(sizeof(MCB)+n);
  // 确定对齐后下一地址
  pte_t *pte=page_walk(current->pagetable,top_heap,0);
  MCB* mcb = (MCB*)(PTE2PA(*pte)+(top_heap&0xfff));
  uint64 addr_mcb = (uint64)mcb;
  uint64 addr_aligned = (addr_mcb)+8-(uint64)addr_mcb % 8;
  mcb = (MCB*)addr_aligned;
  // 分配
  mcb->m_state=1;
  mcb->m_startaddr=top_heap+sizeof(MCB);
  mcb->m_size=n;
  // 插入
  mcb->next=tmp->next;
  tmp->next=mcb;
  return mcb->m_startaddr;
}

// 消去
void user_better_free(void *addr){
  uint64 origin_addr = (uint64)addr-sizeof(MCB);
  pte_t *pte=page_walk(current->pagetable,origin_addr,0);
  MCB* tmp=(MCB*)(PTE2PA(*pte)+(origin_addr&0xfff));
  tmp=(MCB*)((uint64)tmp + 8 - ((uint64)tmp % 8));
  tmp->m_state = 0;
}