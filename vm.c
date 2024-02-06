#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "traps.h"



extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va); //주어진 va를 page align
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1); 
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0) //
      return -1;
    if(*pte & PTE_P)  //현재 page table entry가 present혹은 active 상태임
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

//project 4

struct mmap_area mmap_areas[MAX_MMAP_AREA];
int mmap_area_count = 0;

uint 
mmap(uint addr, int length, int prot, int flags , int fd, int offset){
  
  //===========VALIDATE THE INPUTS===========
  if (mmap_area_count >= MAX_MMAP_AREA) {
    // Maximum number of mmap areas reached
    return 0;
  }
  if(addr % PGSIZE !=0){ //addr이 Page size 와 align되었는지 확인
    return 0;
  }
  if (length <= 0) { // Invalid length
    return 0;
  }
  if (fd == -1 && !(flags & MAP_ANONYMOUS)){ //만약 flags로 받은게 MAP_ANNOYMOUS가 아니다
    return 0;
  }
  if(length % PGSIZE !=0){ //length가 page size와 align 되어있는지 확인
    return 0;
  }
  struct proc *p = myproc();

  //======initialize new area to add======
  struct mmap_area new_area;
  new_area.addr = addr + MMAPBASE;
  new_area.length = length;
  new_area.prot = prot;
  new_area.flags = flags;
  new_area.offset = offset;
  new_area.p = p;

  
  /*FILE MAPPING*/
  if (fd != -1 && !(flags & MAP_ANONYMOUS)) { //fd가 -1이 아니다 && flags가 MAP_ANONYMOUS인 경우

    struct file *file = p->ofile[fd];
    if(file == 0){ //해당 파일 존재X
      return 0;
    }
    new_area.f = file;
    if(!file_is_readable(file) && (prot & PROT_READ)){
      return 0;
    }
    if(!file_is_writable(file) && (prot & PROT_WRITE)){ //file is not readable
      return 0;
    }
    set_offset(file, offset);

    if(MAP_POPULATE & flags){ //file mapping with MAP_POPULATE
      //physical 메모리에 할당하고 page table entry에 저장
      int num_pages = length / PGSIZE;

      if(num_pages > freemem()){
        //not enough free pages
        return 0;
      }

      for(int i=0; i<num_pages; i++){
        char* buffer = kalloc();
        
        if (!buffer) {
          return 0;
        } 
        
        int r = fileread(file, buffer, PGSIZE);
        
        if(r <= 0){
          return 0;
        }
        
        uint va = (MMAPBASE+addr) + i * 4096;

        int cal_prot = 0;
        if(PROT_WRITE & prot){
          cal_prot = PTE_W|PTE_U;
        }else{
          cal_prot = PTE_U;
        }

        if(mappages(p->pgdir, (void *)va, PGSIZE, V2P(buffer), cal_prot) < 0) {
          return 0;
        } 
        //set_offset(file, offset+4096*(i+1));
      }
      
    }//file mapping with MAP_POPULATE
  }/*FILE MAPPING*/

  /*ANNONYMOUS MAPPING with map populate*/
  else if (fd ==-1 && offset == 0 && (flags & (MAP_POPULATE|MAP_ANONYMOUS))){   
    new_area.f = 0;
    int num_pages = length/PGSIZE;
    for(int i=0;i<num_pages;i++){
      char* buffer = kalloc();

      if(!buffer){ //버퍼를 할당받지 못한경우, 이전것 모두 해제
        for (int j = 0; j < i; j++) {
          uint va = (MMAPBASE + addr) + j * PGSIZE;
          kfree((char*)va);
        }
        return 0;
      }

      memset(buffer,0,PGSIZE); //메모리 0으로 initialize
      uint va = (MMAPBASE + addr) + i * PGSIZE;
      
      int cal_prot = 0;
      if(PROT_WRITE & prot){
        cal_prot = PTE_W|PTE_U;
      }else{
        cal_prot = PTE_U;
      }

      if (mappages(p->pgdir, (void *)va, PGSIZE, V2P(buffer), cal_prot) < 0) {
        for (int j = 0; j < i; j++) {
          uint va = (MMAPBASE + addr) + j * PGSIZE;
          kfree((char*)va);
        }
        return 0;
      }
    }
  }/*ANNONYMOUS MAPPING*/

  /*ANNONYmOUSE MAPPING Without map populate*/
  else if (fd ==-1 && offset == 0 && (flags & MAP_ANONYMOUS)){
    new_area.f = 0;
  }
  
  mmap_areas[mmap_area_count]= new_area;
  mmap_area_count++;
  
  return new_area.addr;
}

struct mmap_area* find_mmap_area(uint addr){
  struct proc *p = myproc();
  for(int i=0;i<MAX_MMAP_AREA;i++){
    if(mmap_areas[i].addr == addr && mmap_areas[i].p == p){
      return &mmap_areas[i];
    }
  }
  return 0;
}

void remove_mmap_area(struct mmap_area *target){
  for(int i=0;i<MAX_MMAP_AREA;i++){
    if(&mmap_areas[i] == target){
      memset(&mmap_areas[i], 0, sizeof(struct mmap_area));
      break;
    }
  }
}

int 
munmap(uint addr){
  //return 1 when success
  //return -1 when failed
  //mmap_areas에 존재하는 해당 주소를 갖는 mmap_area를 찾는다
  //그러고 freerange(addr, length+addr)
  struct proc *p = myproc();

  if((addr) % PGSIZE != 0){
    return -1;
  }

  struct mmap_area* area = find_mmap_area(addr);
  if(area == 0){//해당 addr을 가진 mmap_area가 없다
    return -1;
  }
  
  for(uint a = addr; a < addr + area->length; a+=PGSIZE){
    pte_t *pte = walkpgdir(p->pgdir, (void*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a)+1,0,0) - PGSIZE;
    if(pte && (*pte & PTE_P)){ //해당 page가 page table에 있다
      char* phyical_page = P2V(PTE_ADDR(*pte));
      kfree(phyical_page);
      *pte = 0;
    }
  }
  
  remove_mmap_area(area);
  return 1;
}

int page_fault(struct trapframe *tf, uint fault_addr)
{
  fault_addr = PGROUNDDOWN(fault_addr); //접근 주소에 알맞는 페이지를 찾는다
  // mmap_area에 해당주소 매핑되어 있는지 확인
  struct mmap_area *area = 0;
  for(int i = 0; i<MAX_MMAP_AREA;i++){
    if(mmap_areas[i].p == myproc() && mmap_areas[i].addr <= fault_addr && fault_addr <mmap_areas[i].addr+mmap_areas[i].length){
      area = &mmap_areas[i];
      break;
    }
  }
  if (area == 0) //없음
  {
    return -1;
  }

  int write_operation = tf->err & 2; //write인지 확인
  if (write_operation && !(area->prot & PROT_WRITE))
  {//If fault was write while mmap_area is write prohibited, then return -
    return -1;
  }

  pte_t *pte;
  pte = walkpgdir(myproc()->pgdir, (void *)fault_addr, 1);
  if(!pte)
    return -1;
  if (*pte & PTE_P)
  {//page table에 해당 주소가 있는지 확인. 있으면 page fault가 일어나면 안되기 때문에 -1 리턴
    return -1;
  }
  
  char *mem = kalloc();
  if (mem == 0)
  {
    return -1;
  }


  if(!(area->flags & MAP_ANONYMOUS)){//file mapping
    //파일에서 데이터 읽어와서 phyiscal page에 write
    //OFFSET 계산
    
    uint diff = area->offset + fault_addr - area->addr;
    set_offset(area->f,  diff); //현재 page fault가 발생한 파일의 오프셋 결정 
    int r = fileread(area->f, mem, PGSIZE);

    if(r <=0){
      return -1;
    }


  }else{//ANNONYMOUS MAPPING
    memset(mem, 0, PGSIZE); //initialize with 0s
  }

  int cal_prot = 0;
  if(PROT_WRITE & area->prot){
    cal_prot = PTE_W|PTE_U;
  }else{
    cal_prot = PTE_U;
  }
  if (mappages(myproc()->pgdir, (char *)fault_addr, PGSIZE, V2P(mem), cal_prot) < 0)  //mappages 실패
  {//page table에 mapping
    return -1;
  }
  return 0;
}
