/* tiny Musashi disassembler: ./dis <flat.bin> <start_hex> <ninsn> */
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned char MEM[0x40000]; static int SZ;
static unsigned rd8(unsigned a){ a&=0x3ffff; return a<SZ?MEM[a]:0xff; }
unsigned int m68k_read_memory_8(unsigned int a){return rd8(a);}
unsigned int m68k_read_memory_16(unsigned int a){return (rd8(a)<<8)|rd8(a+1);}
unsigned int m68k_read_memory_32(unsigned int a){return ((unsigned)rd8(a)<<24)|((unsigned)rd8(a+1)<<16)|(rd8(a+2)<<8)|rd8(a+3);}
void m68k_write_memory_8(unsigned int a,unsigned int v){}
void m68k_write_memory_16(unsigned int a,unsigned int v){}
void m68k_write_memory_32(unsigned int a,unsigned int v){}
unsigned int m68k_read_disassembler_8(unsigned int a){return rd8(a);}
unsigned int m68k_read_disassembler_16(unsigned int a){return m68k_read_memory_16(a);}
unsigned int m68k_read_disassembler_32(unsigned int a){return m68k_read_memory_32(a);}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: dis <bin> <start_hex> <ninsn>\n");return 1;}
    FILE*f=fopen(argv[1],"rb"); SZ=fread(MEM,1,sizeof MEM,f); fclose(f);
    unsigned pc=strtoul(argv[2],0,16); int n=atoi(argv[3]);
    char buf[160];
    for(int i=0;i<n;i++){ unsigned len=m68k_disassemble(buf,pc,M68K_CPU_TYPE_68000);
        printf("%06x: %s\n",pc,buf); pc+=len; }
    return 0;
}
