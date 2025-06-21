/**-----------------------------------------------------------------------------

 @file    forth.c
 @brief   Implementation of the 'forth' command for HanOS userspace
 @details
 @verbatim


 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>

#include <libc/string.h>
#include <libc/sysfunc.h>
#include <kernel/proc/syscall.h>
//#include <libc/strtoll.h>
uint64_t
strtoll(const char *nptr, char **endptr, register int base);


/* *INDENT-OFF* */
static command_help_t help_msg[] = { 
    {"<help> forth",     "Forth system"},
};
/* *INDENT-ON* */


char* input_buffer[256];

#define STACK_SIZE 1000 /* cells reserved for the stack */
#define RSTACK_SIZE 1000 /* cells reserved for the return stack */
#define HERE_SIZE1 0x10000

typedef uint64_t Cell;
typedef  int64_t sCell;
typedef  void (*proc)(void);

#define pp(cName) const sCell p##cName = (sCell)cName;
#define PP(cName) const sCell P##cName = (sCell)cName;

sCell HereArea[HERE_SIZE1];
// sCell * HereArea;
sCell * StackArea;
sCell * RStackArea;
sCell * here;
sCell *Stack;
sCell *rStack;
sCell * ip ;
sCell ireg;
sCell Tos;
sCell * Handler = NULL;
const char * * ARGVT = NULL;
Cell ARGC;

Cell numericBase = 10;
 
void Co( sCell cod ){ *here++ =  cod ;}
void Co2( sCell cod1,sCell cod2 ){Co(cod1); Co(cod2); }
void Co3( sCell cod1,sCell cod2,sCell cod3 ){Co2(cod1,cod2); ; Co(cod3); }
void Co4( sCell cod1,sCell cod2,sCell cod3,sCell cod4 ){  Co3(cod1,cod2,cod3); ; Co(cod4); }
void Co5( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5 )
{ Co4(cod1,cod2,cod3,cod4); ; Co(cod5); }
void Co6( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5,sCell cod6 )
{ Co5(cod1,cod2,cod3,cod4,cod5); Co(cod6); }
void Co7( sCell cod1,sCell cod2,sCell cod3,sCell cod4,sCell cod5,sCell cod6,sCell cod7 )
{ Co6(cod1,cod2,cod3,cod4,cod5,cod6);  Co(cod7);}

void  Noop(void) {}  pp(Noop)

volatile sCell ppNoop  = pNoop;

void  Hello(void) { printf("Hello Hello !!!\n"); }  pp(Hello)

void  DoVar(void){   *--Stack= Tos; Tos =(sCell)ip; ip = (sCell*)*rStack++; } pp(DoVar)
void  DoConst(void){  *--Stack= Tos; Tos = *ip; ip = (sCell*)*rStack++;
 } pp(DoConst)
void Execute()
    { 
   ireg =Tos; Tos=*Stack++;
	if ( (ireg<0) ^ (ppNoop<0) ) {
	((proc) (~ireg))();
	return;
    	}
    *--rStack = (sCell) ip;
        ip = (sCell *) ireg;
    } pp(Execute)

void DoDefer(){
    ireg = *ip;
    if ( (ireg<0) ^ (ppNoop<0) ) {
        ip = (sCell*)*rStack++; // exit
        ((proc)(~ireg))();
            return;
        }
         ip =  (sCell *) ireg;

} pp(DoDefer)

void Lit_(){  *--Stack = Tos; Tos = *ip++; } pp(Lit_)
void Lit( sCell val) {Co(~pLit_);  *here++ = val;  }
void Compile(){  *here++ = Tos; Tos = *Stack++;  }  pp(Compile)
void LitCo(){  Co(~pLit_); Compile();}  pp(LitCo)
void Allot(){ *(sCell*)&here += Tos; Tos = *Stack++;  }  pp(Allot)

void  Exit() { ip = (sCell*)*rStack++; } pp(Exit)
void  Here(void){  *--Stack = Tos; Tos = (sCell)here;  } pp(Here)

void Branch(){ ip = *(sCell**)ip; } pp(Branch)

void QBranch(){
    if(Tos) ip++;
    else    ip = *(sCell**)ip;
    Tos =   *Stack++;
} pp(QBranch)

void Str() {
    *--Stack = Tos;
    *--Stack = (Cell)ip+1;
        Tos = *(char unsigned *)ip;
    ip = (sCell*) ((Cell)ip + Tos + 1);
    ip = (sCell*) ( ( (Cell)ip + sizeof(Cell) - 1 ) & (-sizeof(Cell) )  );

}  pp(Str)

void Dup(){   *--Stack= Tos;  } pp(Dup)
void Drop(){  Tos = *Stack++;  } pp(Drop)
void Nip(){   Stack++;   } pp(Nip)
void QDup(){   if(Tos) *--Stack= Tos;   } pp(QDup)
void Over(){   *--Stack= Tos; Tos = Stack[1];    } pp(Over)
void Tuck(){    sCell tt=*Stack; *Stack=Tos; *--Stack=tt;  }  pp(Tuck)
void Pick(){    Tos = Stack[Tos];  }  pp(Pick)
void i2dup(){   *--Stack= Tos; *--Stack= Stack[1];  } pp(i2dup)
void i2over(){   *--Stack= Tos;  *--Stack= Stack[3]; Tos= Stack[3];  } pp(i2over)
void i2drop(){  Stack++; Tos = *Stack++; } pp(i2drop)
void Swap(){  sCell tt=Tos; Tos=Stack[0]; Stack[0]=tt;  }  pp(Swap)
void i2Swap(){
    sCell   tt=Tos; Tos=Stack[1]; Stack[1]=tt;
            tt=Stack[0]; Stack[0]=Stack[2]; Stack[2]=tt;  }  pp(i2Swap)
void Rot(){ Cell tt=Stack[1]; Stack[1]=Stack[0]; Stack[0]=Tos; Tos=tt; } pp(Rot)
void Add(){ Tos += *Stack++;  } pp(Add)
void Sub(){ Tos = -Tos;  Tos += *Stack++; } pp(Sub)
void Negate(){ Tos = -Tos; } pp(Negate)
void Invert(){ Tos = ~Tos; } pp(Invert)
void i1Add(){ Tos++; } pp(i1Add)
void i1Sub(){ Tos--; } pp(i1Sub)
void i2Add(){ Tos +=2; } pp(i2Add)
void i2Sub(){ Tos -=2; } pp(i2Sub)
void Mul(){ Tos *= *Stack++; } pp(Mul)
void Div(){ sCell tt=*Stack++; Tos = tt/Tos; } pp(Div)
void i2Mul(){ Tos *= 2; } pp(i2Mul)
void i2Div(){ Tos /= 2; } pp(i2Div)
void Mod(){ sCell tt=*Stack++; Tos = tt%Tos; } pp(Mod)
void UMul(){ Tos = (Cell) Tos * (Cell) *Stack++; } pp(UMul)
void UDiv(){ Cell tt=*Stack++; Tos = tt/(Cell)Tos; } pp(UDiv)
void And(){ Tos &= *Stack++; } pp(And)
void AndC(){ Tos = ~Tos & *Stack++; } pp(AndC)
void Or(){  Tos |= *Stack++; } pp(Or)
void Xor(){ Tos ^= *Stack++; } pp(Xor)
void ARshift(){  Tos = *Stack++ >> Tos ; } pp(ARshift)
void Rshift(){  Tos = *(Cell*)Stack++ >> Tos ; } pp(Rshift)
void Lshift(){  Tos = *Stack++ << Tos ; } pp(Lshift)

void HDot(){
    printf("%llx ",Tos);
    Tos = *Stack++;
}   pp(HDot)

void UDot() {
    uint8_t buffer [44];
    uint8_t* p = &buffer;

    size_t s = Tos;

    do {
        ++p;
        s = s / numericBase;
    } while(s);

    *p = ' ';
    p[1] = '\0';

    do {
	uint8_t nn= (Tos % numericBase);
	if(nn<10)  *--p = '0' + nn;
	else  *--p = 'A' + nn - 10 ;
        Tos = Tos / numericBase;
    } while(Tos);
    sys_write(STDOUT,buffer,strlen(buffer));
    Tos = *Stack++; 
}   pp(UDot)

void Dot() {
 if(Tos<0){sys_write(STDOUT,"-",1); Negate();} 
 UDot();
}   pp(Dot)

void Load(){  Tos =  *(Cell*)Tos;  } pp(Load)
void Store(){ *(Cell*)Tos = *Stack++;  Tos = *Stack++;} pp(Store)
void CLoad(){ Tos =  (Cell)*(uint8_t*) Tos;  } pp(CLoad)
void CStore(){ *(uint8_t*)Tos = (uint8_t)*Stack++; Tos = *Stack++;  } pp(CStore)
void CAddStore(){ *(uint8_t*)Tos += (uint8_t)*Stack++; Tos = *Stack++;  } pp(CAddStore)
void CStoreA(){ *(uint8_t*)Tos = (uint8_t)*Stack++; } pp(CStoreA)
void WLoad(){ Tos =  (Cell)*(uint16_t*) Tos;  } pp(WLoad)
void WStore(){ *(uint16_t*)Tos = (uint16_t)*Stack++; Tos = *Stack++;  } pp(WStore)

void i2Store(){
 uint64_t val = ((uint64_t)(Cell)Stack[1]<<32) + (uint64_t)(Cell)Stack[0];
   *(uint64_t*)Tos = val;
   Stack += 2 ;  Tos = *Stack++;} pp(i2Store)

void i2Load(){  uint64_t val = *(uint64_t*)Tos; Tos= val; *--Stack=val>>32;} pp(i2Load)

void AddStore(){ *(Cell*)Tos += *Stack++;  Tos = *Stack++;} pp(AddStore)
void Count(){ *--Stack = Tos+1; Tos = (sCell) *(char *)Tos; } pp(Count)
void On(){  *(Cell*)Tos = -1; Tos = *Stack++; } pp(On)
void Off(){ *(Cell*)Tos = 0; Tos = *Stack++;  } pp(Off)
void Incr(){  *(Cell*)Tos += 1; Tos = *Stack++; } pp(Incr)
void ZEqual(){ Tos = -(Tos==0); } pp(ZEqual)
void ZNEqual(){ Tos = -(Tos!=0); } pp(ZNEqual)
void DZEqual(){  Tos = -( (Tos | *Stack++) == 0); } pp(DZEqual)
void ZLess(){ Tos = -(Tos<0); } pp(ZLess)
void Equal(){  Tos = -(*Stack++==Tos); } pp(Equal)
void NEqual(){  Tos = -(*Stack++!=Tos); } pp(NEqual)
void Less(){   Tos = -(*Stack++<Tos);  } pp(Less)
void Great(){  Tos = -(*Stack++>Tos);  } pp(Great)
void ULess(){  Tos = -((Cell)*Stack++ < (Cell)Tos); } pp(ULess)
void UGreat(){ Tos = -((Cell)*Stack++ > (Cell)Tos); } pp(UGreat)

void Max(){ sCell tt = *Stack++; if(tt>Tos) Tos=tt; } pp(Max)
void Min(){ sCell tt = *Stack++; if(tt<Tos) Tos=tt; } pp(Min)
void i0Max(){  if(Tos<0) Tos=0; } pp(i0Max)

void ToR(){   *--rStack = Tos; Tos = *Stack++; }	pp(ToR)
void RLoad(){ *--Stack = Tos; Tos = *rStack; }		pp(RLoad)
void FromR(){ *--Stack = Tos; Tos = *rStack++; }	pp(FromR)
void i2ToR(){  *--rStack = *Stack++; *--rStack = Tos ; Tos = *Stack++; } pp(i2ToR)
void i2RLoad(){ *--Stack = Tos; Tos = *rStack; *--Stack = rStack[1];	  } pp(i2RLoad)
void i2FromR(){ *--Stack = Tos; Tos = *rStack++; *--Stack = *rStack++;	  } pp(i2FromR)
void RDrop(){ *rStack++; }    pp(RDrop)
void RPGet(){ *--Stack = Tos; Tos = (Cell) rStack; } pp(RPGet)
void SPGet(){ *--Stack = Tos; Tos = (Cell) Stack ; } pp(SPGet)
void RPSet(){   rStack = (sCell*)Tos; Tos = *Stack++; } pp(RPSet)
void SPSet(){    Stack = (sCell*)(Tos+sizeof(sCell)); Tos = Stack[-1]; } pp(SPSet)

void ZCount(){ *--Stack= Tos; Tos = strlen((char *)Tos); } pp(ZCount)

void Emit() {
	sys_write(STDOUT,(char*)&Tos,1);
	Tos = *Stack++; } pp(Emit)

void Space() {
	sys_write(STDOUT," ",1);
 } pp(Space)

void Cr() { sys_write(STDOUT,"\n",1); } pp(Cr)

void Type() {
	sys_write(STDOUT,(char*)*Stack++,Tos);
	Tos =  *Stack++;
} pp(Type)

void ZType() {
	sys_write(STDOUT,(char *)Tos,strlen((char *)Tos));
	Tos =  *Stack++;} pp(ZType)

void Ahead(){ Co(~pBranch); *--Stack = Tos; Tos = (sCell)here; Co(0);} pp(Ahead)
void If(){ Co(~pQBranch); *--Stack = Tos; Tos= (sCell)here; Co(0);} pp(If)
void Then(){  *(sCell**)Tos++ = here; Tos = *Stack++; } pp(Then)
void Else(){  Ahead();    Swap(); Then(); } pp(Else)
void Begin(){ *--Stack = Tos; Tos =  (sCell)here; } pp(Begin)
void Until(){ Co(~pQBranch);   *here++ = (sCell)Tos; Tos = *Stack++; } pp(Until)
void Again(){ Co(~pBranch);  *here++ = (sCell)Tos; Tos = *Stack++; } pp(Again)
void While(){ If(); Swap(); } pp(While)
void Repeat(){ Again(); Then(); }   pp(Repeat)

void DNegate(){ int64_t val =
 -(int64_t)( ((uint64_t)(Cell)Tos<<32) + (uint64_t)(Cell)Stack[0] ) ;
	Tos= val>>32;
	Stack[0]=val;
  } pp(DNegate)

void DAbs(){   if(Tos<0) DNegate();  } pp(DAbs)

void DAdd()
{ uint64_t sum= ((uint64_t)(Cell)Tos<<32) + (uint64_t)(Cell)Stack[0] +
	 ((uint64_t)(Cell)Stack[1]<<32) + (uint64_t)(Cell)Stack[2];
	Stack += 2 ;
	Tos=(sCell) sum>>32;
	Stack[0]=sum;
} pp(DAdd)

void UMMul()
{ uint64_t mul= (uint64_t)(Cell)Tos * (uint64_t)(Cell)Stack[0] ;
	Tos= mul>>32;
	Stack[0]=mul;
} pp(UMMul)

/* Divide 64-bit unsigned number (high half *b, low half *c) by
   32-bit unsigend number in *a. Quotient in *b, remainder in *c.
*/
static void udiv(uint64_t a,uint64_t *b,uint64_t *c)
{
 uint64_t d,qh,ql;
 int i,cy;
 qh=*b;ql=*c;d=a;
 if(qh==0) {
  *b=ql/d;
  *c=ql%d;
 } else {
  for(i=0;i<64;i++) {
   cy=qh&0x8000000000000000;
   qh<<=1;
   if(ql&0x8000000000000000)qh++;
   ql<<=1;
   if(qh>=d||cy) {
    qh-=d;
    ql++;
    cy=0;
   }
   *c=qh;
   *b=ql;
  }
 }
}
void UMMOD()
{	if(Tos<=*Stack) { /*overflow */
	*++Stack=-1;
	 Tos = -1; return;
        }
        udiv(Tos,Stack,&Stack[1]);
        Tos = *Stack++;
} pp(UMMOD)

void DIVMOD() // n1 n2 -- rem quot
{	sCell tt=*Stack;
        *Stack = tt%Tos ;
	Tos = tt/Tos;
} pp(DIVMOD)

void Align()
{   Cell sz = ( sizeof (Cell) - 1 ) ;
    char * chere = (char *)here;
    while( (Cell) chere & sz ) *chere++ = 0 ;
    here = (sCell *)chere;
}
// CODE FILL ( c-addr u char -- ) \ 94
void Fill()
{    Cell len =  *Stack++;
    uint8_t *adr = (uint8_t *) *Stack++;
  while (len-- > 0)  *adr++ = (uint8_t)Tos;
  Tos =  *Stack++;
}  pp(Fill)

void Cmove()
{
  uint8_t *c_to = (uint8_t *) *Stack++;
  uint8_t *c_from =(uint8_t *) *Stack++;
  while (Tos-- > 0)
    *c_to++ = *c_from++;
  Tos =  *Stack++;
}  pp(Cmove)

void Cmove_up()
{
  uint8_t *c_to = (uint8_t *) *Stack++;
  uint8_t *c_from =(uint8_t *) *Stack++;
  while (Tos-- > 0)
    c_to[Tos] = c_from[Tos];
  Tos =  *Stack++;
}  pp(Cmove_up)

void StrComp(const char * s, sCell len)
{   char * chere = (char *)here;
    len &= 0xff ;
    *chere++ = (char)len;                /* store count byte */
    while (--len >= 0)          /* store string */
        *chere++ = *s++;

    here = (sCell *)chere;
    Align();
}

void StrCmp(){  StrComp((char *) *Stack++, Tos); Tos = *Stack++; } pp(StrCmp)

void Tp(const char * s) {
    Co(~pStr);
    StrComp(s, strlen(s));
    Co(~pType);
}

void SpSet(){    Stack = (sCell*)*Stack; } pp(SpSet)

sCell  ForthWordlist[] = {0,0,0};

#define ContextSize 10
sCell * Context[ContextSize] = {ForthWordlist};
sCell * Current[] = {ForthWordlist};

sCell * Last;
sCell * LastCFA;


void  WordBuild (const char * name, sCell cfa )
{
    LastCFA=here;
    Co(cfa);
    Co(0); // flg
    Co(** (sCell **) Current);
    Last=here;
    StrComp(name, strlen(name));
}

void Smudge(){ **(sCell***) Current=Last; } pp(Smudge)

void Immediate(){ Last[-2] |= 1; } pp(Immediate)

void FthItem (const char * name, sCell cfa ){
    WordBuild (name, cfa );
    Smudge();
}

sCell Header(const char * name) {
    FthItem (name,0);
    *(sCell **)LastCFA = here;
    return  *(sCell *)LastCFA;
}

sCell Variable (const char * name ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ = ~pDoVar;
    *here++ = 0;
    return  *(sCell *)LastCFA;
}

sCell VVariable (const char * name, sCell val ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ = ~pDoVar;
    *here++ = val;
    return  *(sCell *)LastCFA;
}

sCell Constant (const char * name, sCell val ) {
    FthItem(name,0);
    *(sCell **) LastCFA = here;
    *here++ = ~pDoConst;
    *here++ = val;
    return  *(sCell *)LastCFA;
}
char atib[256]={"atib atib qwerty"};
sCell tib[]={0,(sCell)atib}; PP(tib)
sCell ntib;
void Source(){
 *--Stack = Tos;
 *--Stack = tib[1];
    Tos =  ntib;
  } pp(Source)

void SourceSet(){
  ntib = Tos;
 tib[1] = *Stack++;
 Tos = *Stack++;
  } pp(SourceSet)

// ALLOCATE ( u -- a-addr ior ) 
void Allocate()
{
	*--Stack= Tos;
	
	*Stack= (sCell) sys_malloc(Tos);
	Tos=0;
  	if(*Stack==0) Tos=-59;

} pp(Allocate)

void Free()
{
	//sys_free(Tos);
	Tos=0;

} pp(Free)
/*
void GetARGVT()
{	*--Stack= Tos;
	*--Stack=(sCell)ARGV1;
	Tos=strlen(ARGV1);
} pp(GetARGV)
*/
sCell i2in[] = {0 , 0  }; PP(i2in)
sCell *v2in = (sCell *) &i2in[1];

sCell SourceId[] = { 0, 0 }; PP(SourceId)

#define CMD_PROMPT      "\033[36mO \033[0m"

void Accept() // ( c-addr +n -- +n' )
{
 int nbuf;
     char *buf = *Stack++;
     char save[2]={'\b'};
    int i;
	    for (i = 0;;) { save[1] = buf[i];
        if (sys_read(STDIN, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == '\b') {
            if (i > 0) i--;
            continue;
        }
        if (buf[i] == 0x1b) { buf[i++] = save[1];
		sys_write(STDOUT, &save, 2);
            continue;
        }

        if (i >= Tos - 1) break;
        if (buf[i] == (char) EOF) {  Tos=0; return;}
        if (buf[i] == '\n') break;
        i++;
    }
	Tos=i;
} pp(Accept)
const char *initScript;
void ParseName() {
    Cell addr,Waddr,Eaddr;
    addr=  tib[1] + *v2in;
    Eaddr= tib[1] + ntib;
    *--Stack = Tos;
    while (  addr<Eaddr ) {
 if( *(uint8_t*)addr > ' ') break;
        addr++;
  }
    *--Stack=Waddr=addr;
    *v2in = addr - tib[1];
    while ( addr<=Eaddr ) {
 (*v2in)++; if( *(uint8_t*)addr <= ' ') break;
     addr++; }
    Tos=addr-Waddr;
} pp(ParseName)

void Parse() {
    Cell addr,Waddr,Eaddr;
	if(((uint8_t*)tib[1])[ntib] == '\r' ) ntib--;
    addr=  tib[1]  + *v2in;
    Eaddr= tib[1]  + ntib;

    char cc = (char)Tos;
    *--Stack=Waddr=addr;
    while ( addr<=Eaddr ) {  (*v2in)++;  if(*(uint8_t*)addr == cc ) break;
        addr++;}
    Tos=addr-Waddr;
} pp(Parse)

Cell memcasecmp (const void *vs1, const void *vs2, Cell n)
{
    unsigned int i;
    uint8_t const *s1 = (uint8_t const *) vs1;
    uint8_t const *s2 = (uint8_t const *) vs2;
    for (i = 0; i < n; i++)
    {
        uint8_t u1 = *s1++;
        uint8_t u2 = *s2++;
        if (toupper (u1) != toupper (u2))
            return toupper (u1) - toupper (u2);
    }
    return 0;
}

Cell CCompare( void * caddr1  ,  Cell len1 ,  void * caddr2  ,  Cell len2) {
    if (len1 < len2) return -1;
    if (len1 > len2) return  1;

    auto int cmpResult = memcasecmp(caddr1, caddr2, len1);

    if (cmpResult < 0) return -1;
    if (cmpResult > 0) return  1;
    return   0;
}

void UCompare(){ 
	char * caddr1 = (char *) *Stack++;
	sCell  len1 =  *Stack++;
	char * caddr2 = (char *) *Stack++;

    if (len1 != Tos) {  Tos -= len1; return; }

    Tos = memcasecmp(caddr1, caddr2, Tos);  } pp(UCompare)

char *SEARCH(char **wid,  char * word , Cell len)
{ char * addr= (char *) *wid;
    for(;;)
    {   if(!addr) return NULL;
        char * caddr = addr ;
        if( !CCompare(word, len, caddr+1, *caddr ))
            return  addr;
        addr = ((char **)addr)[-1];
    }
}

void FromName(){  Tos=((sCell *)Tos)[-3]; } pp(FromName)

void SearchWordList() // ( c-addr u wid --- 0 | xt 1 xt -1 )
{
    char ** addr=  (char **) Tos;
    Cell  len=Stack[0];
    char * word= (char * ) Stack[1];

    if(!addr) { Stack+=2; Tos=0; return; }
    Cell * nfa= (Cell*) SEARCH(addr,word,len);
    if(!nfa) {
        Stack+=2; Tos=0;
        return;
    }
    Stack++;
    Stack[0]=nfa[-3];
    Tos = nfa[-2]&1 ? 1 : -1;

}  pp(SearchWordList)

void SFind()
{	sCell * voc=  (sCell *) Context;
    *--Stack = Tos;
    while( *voc )
    {	*--Stack = Stack[1];
        *--Stack = Stack[1]; Tos=*voc;
        SearchWordList();
        if(Tos)
        {   Stack[2]=Stack[0];  Stack+=2; // 2nip
            return;
        }   voc++;
    }

} pp(SFind)

Cell State;

void StateQ(){ *--Stack= Tos; Tos = State; } pp(StateQ)

void IMode(){ State = 0;}  pp(IMode)
void CMode(){ State = -1;}  pp(CMode)

sCell * YDP;
sCell * YDP0;

sCell YDPFL[] = { pDoConst, 0 }; pp(YDPFL)

void QYDpDp()
{
  if(YDPFL[1] == 0) return;
   sCell * tmp = YDP ;
    YDP = here ;
    here = tmp ;
}

void SBuild()
{    char * name = (char * ) *Stack++ ;
	QYDpDp();
    LastCFA=here;
    Co(0);
    Co(0); // flg
    Co(** (sCell  **) Current);
    Last=here;
    StrComp(name, Tos);
    Tos = *Stack++;
	QYDpDp();
    *(sCell **)LastCFA = here;
}

void Build()
{   ParseName();
    SBuild();
} pp(Build)

void SHeader()
{
	SBuild();
	Smudge();  
} pp(SHeader)

void SNumber0() // ( str len -- m flg )
{
    char* rez;
    char  NumStr[44];
    sCell signedFlg = 1;
    Cell len = Tos;
    char * caddr = (char*) Stack[0];
    if(caddr[0]=='-') { len--; caddr++; signedFlg = -1; }
    NumStr[len]=0;
    while(len){ --len; NumStr[len] = caddr[len]; }
    *Stack =(sCell)strtoll( NumStr,  &rez, numericBase) * signedFlg;
    Tos =  strlen(rez);
}  pp(SNumber0)

void Colon(){
  Build();
  CMode(); } pp( Colon)
void Semicolon(){ Co(~pExit); Smudge(); IMode(); } pp(Semicolon)

void to_catch(){
    *--rStack = (sCell)Handler;
    *--rStack = (sCell)Stack;
    Handler = rStack;
    Execute();
} pp(to_catch)

void from_catch(){
    rStack++;
    Handler = (sCell*)*rStack++;
    *--Stack = Tos;  Tos = 0;
    ip = (sCell*)*rStack++; // exit
} pp(from_catch)

sCell Catch[] = { 0,0 }; PP(Catch)

void FThrowDo()
{   *--Stack = Tos;
    if (Handler == NULL); //  TODO("Handler=0")
    rStack =   Handler ;
    Stack = (sCell*)*rStack++;
    Handler = (sCell*)*rStack++;
    ip = (sCell * ) *rStack++;
}

void FThrow(){
    if (Tos == 0){  Tos = *Stack++; return;  }
    FThrowDo();
} pp(FThrow)

sCell Lastin =0;
sCell SaveErrQ = -1;
sCell ErrIn;

void SaveErr0()
{ if(SaveErrQ & Tos )
    {  SaveErrQ = 0;
       ErrIn = *v2in ;
    }

} pp(SaveErr0)

void PrintErr0()
{  numericBase = 10;
     printf("Err=%d\n",Tos);
     Tos = *Stack++;
     SaveErrQ=-1;
} pp(PrintErr0)

// R/O ( -- fam )
void readOnly() { *--Stack = Tos; Tos = O_RDONLY; }  pp(readOnly)

// R/W ( -- fam )
void readWrite() { *--Stack = Tos; Tos = O_RDWR; } pp(readWrite)

// W/O ( -- fam )
void writeOnly() { *--Stack = Tos; Tos = O_WRONLY ; } pp(writeOnly)

// OPEN-FILE ( c-addr u fam -- fileid ior )

void openFile() {
  char path[100];
  sCell uu = *Stack++;
  char * caddr = (char *) *Stack;
  path[uu]=0;
  while(uu){--uu;  path[uu]=caddr[uu];}

    *Stack = sys_open(path, Tos);
    Tos = *Stack < 0;

} pp(openFile)

void closeFile() {  Tos =  sys_close(Tos); } pp(closeFile)

// READ-FILE ( c-addr u1 fileid -- u2 ior )
void readFile() {

    Cell len = *Stack++;
    char * buffer = (char*) *Stack;
	
    *Stack = sys_read(Tos, buffer, len);

    Tos = *Stack < 0;

} pp(readFile)

// READ-LINE ( c-addr u1 fileid -- u2 flag ior )
void readLine() {
    Cell nbuf = *Stack;
    char * buf = (char*)Stack[1];
    int i = 0;
    for (;;i++) {

	if(sys_read(Tos, &buf[i], 1) != 1){
        	Stack[1] = i;
		*Stack= -(i!=0) ;
		Tos=0;	return;}

        if (i >= nbuf - 1)
            break;
        if (buf[i] == '\n')
            break;
        if (buf[i] == (char) EOF) { 
	        Stack[1] = 0;
		*Stack=0;
		Tos=0;	return;}
        }


        Stack[1] = i;

	*Stack=-1;
	Tos=0;

} pp(readLine)

// WRITE-FILE ( c-addr u1 fileid -- ior )
void writeFile() {
	
    Cell len = *Stack++;
    char * buffer = (char*) *Stack++;

    Tos = sys_write( Tos, buffer , len) < 0 ;

} pp(writeFile)

void  Bye(void) { sys_exit(0); } pp(Bye)

int fork1(void)
{
    int pid;

    pid = sys_fork();
    if (pid == -1)
        sys_panic("fork");
    return pid;
}

typedef struct {
    int type;
} cmd_t;


const char *initScript =
       " : 2NIP 2SWAP 2DROP ; \n"
        " : COMPILE, , ;\n"
        " : HEX 16 BASE ! ;\n"
        ": DECIMAL 10 BASE ! ; \n"
        ": HEADER BUILD SMUDGE ;\n"
        ": CONSTANT HEADER DOCONST , , ;\n"
        ": CREATE HEADER DOVAR , ;\n"
        ": VARIABLE CREATE 0 , ;\n"
        ": [COMPILE] ' , ; IMMEDIATE\n"
        ": CELL+ CELL + ;\n"
        ": CELL- CELL - ;\n"
        ": CELLS CELL * ;\n"
        ": >BODY CELL+ ;\n"
        ": COMPILE R> DUP @ , CELL+ >R ;\n"
        ": CHAR  PARSE-NAME DROP C@ ;\n"
        ": [CHAR] CHAR LIT,  ; IMMEDIATE\n"
        ": [']  ' LIT, ; IMMEDIATE\n"
        ": .( [CHAR] ) PARSE TYPE ; IMMEDIATE\n"
        ": ( [CHAR] ) PARSE 2DROP ; IMMEDIATE\n"
        ": SLIT, ( string -- ) COMPILE <$> $, ;\n"
        ": \\ 10 PARSE 2DROP  ; IMMEDIATE\n"
        ": .\\ 10 PARSE TYPE cr ; IMMEDIATE\n"
        ": .\" [CHAR] \" PARSE SLIT, COMPILE TYPE   ; IMMEDIATE\n"
        ": S\" [CHAR] \" PARSE ?STATE IF SLIT, THEN ; IMMEDIATE\n"
        ": ABORT -1 THROW ;\n"
        ": POSTPONE\n" // 94
        "  PARSE-NAME SFIND DUP\n"
        "  0= IF -321 THROW THEN \n"
        "  1 = IF COMPILE,\n"
        "      ELSE LIT, ['] COMPILE, COMPILE, THEN\n"
        "; IMMEDIATE\n"
        ": TO '\n"
        "   ?STATE 0= IF >BODY ! EXIT THEN\n"
        "    >BODY LIT, POSTPONE ! ; IMMEDIATE\n"
	": ERASE 0 FILL ;\n"
	": $!\n" //	( addr len dest -- )
	"SWAP 255 AND SWAP	2DUP C! 1+ SWAP CMOVE ;\n"
        ": DEFER@  ( xt1 -- xt2 )  >BODY @ ;\n"
        ": VALUE CONSTANT ;\n"
        ": (DO)   ( n1 n2 ---)\n"
        // Runtime part of DO.
        " R> ROT ROT SWAP >R >R >R ;\n"
        ": (?DO)  ( n1 n2 ---)\n"
        // Runtime part of ?DO
        "  OVER OVER - IF R> ROT ROT SWAP >R >R CELL+ >R \n"
        "                 ELSE DROP DROP R> @ >R\n" // Jump to leave address if equal
        "                 THEN ;\n"
        ": I ( --- n )\n"
        // Return the counter (index) of the innermost DO LOOP
        "  POSTPONE R@ ; IMMEDIATE\n"
                ": z\\ 10 PARSE h. h. ; IMMEDIATE\n"

        ": J  ( --- n)\n"
        // Return the counter (index) of the next loop outer to the innermost DO LOOP
        " RP@ 3 CELLS + @ ;\n"
        "VARIABLE 'LEAVE ( --- a-addr)\n" // This variable is  used  for  LEAVE address resolution.

        ": (LEAVE)   ( --- )\n"
        // Runtime part of LEAVE
        " R> @ R> DROP R> DROP >R ;\n" // Remove loop parameters and replace top of ret\n"
        // stack by leave address.\n"

        ": UNLOOP ( --- )\n"
        // Remove one set of loop parameters from the return stack.
        "   R> R> DROP R> DROP >R ;\n"

        ": (LOOP) ( ---)\n"
        // Runtime part of LOOP
        "  R> R> 1+ DUP R@ = \n"   // Add 1 to count and compare to limit.
        "  IF \n"
        "   R> DROP DROP CELL+ >R\n" // Discard parameters and skip leave address.
        "  ELSE \n"
        "   >R @ >R\n" // Repush counter and jump to loop start address.
        "  THEN ;\n"

        ": (+LOOP) ( n ---)\n"
        // Runtime part of +LOOP
        // Very similar to (LOOP), but the compare condition is different.
        //  exit if ( oldcount - lim < 0) xor ( newcount - lim < 0).
        "     R> SWAP R> DUP R@ - ROT ROT + DUP R@ - ROT XOR 0 < \n"
        "     IF R> DROP DROP CELL+ >R\n"
        "     ELSE >R @ >R THEN ;\n"

        ": DO ( --- x)\n"
        // Start a DO LOOP.
        // Runtime: ( n1 n2 --- ) start a loop with initial count n2 and
        // limit n1.
        "  POSTPONE (DO) 'LEAVE @  HERE 0 'LEAVE ! \n"
        "   ; IMMEDIATE\n"

        ": ?DO  ( --- x )\n"
        // Start a ?DO LOOP.\n"
        // Runtime: ( n1 n2 --- ) start a loop with initial count n2 and
        // limit n1. Exit immediately if n1 = n2.
        "  POSTPONE (?DO)  'LEAVE @ HERE 'LEAVE ! 0 , HERE ; IMMEDIATE\n"

        ": LEAVE ( --- )\n"
        // Runtime: leave the matching DO LOOP immediately.
        // All places where a leave address for the loop is needed are in a linked\n"
        // list, starting with 'LEAVE variable, the other links in the cells where
        // the leave addresses will come.
        "  POSTPONE (LEAVE) HERE 'LEAVE @ , 'LEAVE ! ; IMMEDIATE\n"
        ": RESOLVE-LEAVE\n"
        // Resolve the references to the leave addresses of the loop.
        "         'LEAVE @\n"
        "         BEGIN DUP WHILE DUP @ HERE ROT ! REPEAT DROP ;\n"

        ": LOOP  ( x --- )\n"
        // End a DO LOOP.
        // Runtime: Add 1 to the count and if it is equal to the limit leave the loop.
        " POSTPONE (LOOP) ,  RESOLVE-LEAVE  'LEAVE ! ; IMMEDIATE\n"

        ": +LOOP  ( x --- )\n"
        // End a DO +LOOP
        // Runtime: ( n ---) Add n to the count and exit if this crosses the
        // boundary between limit-1 and limit.
        " POSTPONE (+LOOP) , RESOLVE-LEAVE 'LEAVE ! ; IMMEDIATE\n"

        ": (;CODE) ( --- )\n"
        // Runtime for DOES>, exit calling definition and make last defined word
        // execute the calling definition after (;CODE)
        "  R> LAST @  NAME>  ! ;\n"

        ": DOES>  ( --- )\n"
        // Word that contains DOES> will change the behavior of the last created
        // word such that it pushes its parameter field address onto the stack
        // and then executes whatever comes after DOES>
        " POSTPONE (;CODE) \n"
        " POSTPONE R>\n" // Compile the R> primitive, which is the first
        // instruction that the defined word performs.
        "; IMMEDIATE\n"

    ": SET-CURRENT ( wid -- )\n" // 94 SEARCH
    "        CURRENT ! ;\n"

    ": GET-CURRENT ( -- wid )\n" // 94 SEARCH
    "        CURRENT @ ;\n"

    ": GET-ORDER ( -- widn ... wid1 n )\n"  // 94 SEARCH
        " SP@ >R 0 >R\n"
        " CONTEXT\n"
        " BEGIN DUP @ ?DUP\n"
        " WHILE >R CELL+\n"
        " REPEAT  DROP\n"
        " BEGIN R> DUP 0=\n"
        " UNTIL DROP\n"
        "R> SP@ - CELL / 1- ; \n"

	" HERE S\" FORTH\" $, FORTH-WORDLIST CELL+ !\n"

        ": VOC-NAME. ( wid -- )\n"
        "DUP CELL+ @ DUP IF COUNT TYPE BL EMIT DROP ELSE DROP .\" <NONAME>:\" U. THEN ;\n"

        ": ORDER ( -- )\n" // 94 SEARCH EXT
        "GET-ORDER .\" Context: \" \n"
        "0 ?DO ( DUP .) VOC-NAME. SPACE LOOP CR\n"
        ".\" Current: \" GET-CURRENT VOC-NAME. CR ;\n"

        ": SET-ORDER ( wid1 ... widn n -- )\n"
        "DUP -1 = IF\n"
        "DROP  FORTH-WORDLIST 1\n"
        "THEN\n"
        "DUP  CONTEXT-SIZE  U> IF -49 THROW THEN\n"
        "DUP CELLS context + 0!\n"
        "0 ?DO I CELLS context + ! LOOP ;\n"
        "CREATE VOC-LIST FORTH-WORDLIST CELL+ CELL+ ,\n"

        ": FORTH FORTH-WORDLIST CONTEXT ! ;\n"
        ": DEFINITIONS  CONTEXT @ CURRENT ! ;\n"

        ": WORDLIST ( -- wid )\n" // 94 SEARCH
        " HERE 0 , 0 , \n"
        " HERE VOC-LIST  @ , .\" W=\" DUP H.  VOC-LIST ! ;\n"

	": ONLY ( -- ) -1 SET-ORDER ;\n"
	": ALSO ( -- )   GET-ORDER OVER SWAP 1+ SET-ORDER ;\n"
	": PREVIOUS ( -- ) GET-ORDER NIP 1- SET-ORDER ;\n"


	": LATEST ( -> NFA ) CURRENT @ @ ;\n"

	": VOCABULARY ( <spaces>name -- )\n"
	"WORDLIST CREATE DUP ,\n"
	"LATEST SWAP CELL+ !\n"
	"DOES>  @ CONTEXT ! ;\n"
	" VARIABLE CURSTR\n"

	": ->DEFER ( cfa <name> -- )  HEADER DODEFER , , ;\n"
	": DEFER ( <name> -- ) ['] ABORT ->DEFER ;\n"

        ": VECT DEFER ;\n"

	": FQUIT  BEGIN REFILL WHILE CURSTR 1+!\n"
	"  INTERPRET  REPEAT ;\n"

	": LALIGNED  3 + 3 ANDC ;\n"

	" 255 CONSTANT TC/L\n"

 ": INCLUDE-FILE\n" // ( fid --- )
// Read lines from the file identified by fid and interpret them.
// INCLUDE and EVALUATE nest in arbitrary order.
	"SOURCE-ID >R >IN @ >R LASTIN @ >R CURSTR @ >R CURSTR 0!\n"
	"SOURCE 2>R\n"
	" TC/L ALLOCATE THROW TC/L SOURCE!\n"
	"TO SOURCE-ID\n"
	"['] FQUIT CATCH SAVEERR\n"
	"TIB FREE DROP\n"
	"2R> SOURCE!\n"

	"R> CURSTR ! R> LASTIN ! R> >IN ! R> TO SOURCE-ID\n"
	"THROW ;\n"

	": FREFILL0\n" // (  -- flag )
	"  TIB TC/L SOURCE-ID READ-LINE THROW\n"
	"  SWAP  #TIB !  0 >IN ! CURSTR 1+!\n"
	"  0 SOURCE + C! ;\n"
	"' FREFILL0 TO FREFILL\n"

  "444 CONSTANT  CFNAME_SIZE\n"
  "CREATE CURFILENAME  CFNAME_SIZE 255 + 1+ ALLOT\n"
  "CURFILENAME  CFNAME_SIZE 255 + 1+  ERASE\n"

  ": CFNAME-SET\n" // ( adr len -- )
  "DUP 1+ >R  CURFILENAME CURFILENAME R@ + CFNAME_SIZE R> - CMOVE>\n"
  "CURFILENAME $! ;\n"

  ": CFNAME-FREE\n" //  ( -- )
  "CURFILENAME COUNT + CURFILENAME\n"
  "CFNAME_SIZE CURFILENAME C@ - 255 +  CMOVE ;\n"

 ": INCLUDED\n" // ( c-addr u ---- )
 "2DUP CFNAME-SET\n"
 "R/O OPEN-FILE THROW\n"
 "DUP >R ['] INCLUDE-FILE CATCH\n"
 "DUP IF cr .\" in <\" CURFILENAME COUNT TYPE .\" >\" CURSTR @ . THEN  CFNAME-FREE\n"
 "R> CLOSE-FILE DROP THROW ;\n"
 
 ": INTERPRET2\n"
 "  CR   Begin  \n"
 "        9 9 + H. >IN @ DUP H. Lastin ! SAVEERR? On\n"
 "        PARSE-NAME .\" <\" 2DUP TYPE .\" >\"  Dup\n"
 "    While SFind ?Dup\n"
 "        If  ?State =\n"
 "             If Compile,\n"
 "            Else Execute\n"
 "            Then\n"
 "        Else ?SLiteral\n"
 "        Then ?STACK\n"
 "    Repeat ;\n"

// " ' INTERPRET2 TO INTERPRET \n"
// " INTERPRET2 \n"
 ": EVALUATE\n" // ( i*x c-addr u -- j*x ) \ 94
 "SOURCE-ID >R SOURCE 2>R >IN @ >R\n"
 "-1 TO SOURCE-ID\n"
 "SOURCE! >IN 0!\n"
 "['] INTERPRET CATCH\n"
 "R> >IN ! 2R> SOURCE! R> TO SOURCE-ID\n"
 "THROW ;\n"

 ": FLOAD PARSE-NAME INCLUDED ;\n"

 ": [DEFINED]\n" //  ( -- f ) \ "name"
 "PARSE-NAME  SFIND  IF DROP -1 ELSE 2DROP 0 THEN ; IMMEDIATE\n"

 ": [UNDEFINED]\n" //  ( -- f ) \ "name"
 "POSTPONE [DEFINED] 0= ; IMMEDIATE\n"

 ": \\+	POSTPONE [UNDEFINED]	IF POSTPONE \\ THEN ; IMMEDIATE\n"
 ": \\-	POSTPONE [DEFINED]	IF POSTPONE \\ THEN ; IMMEDIATE\n"

 ": BREAK  POSTPONE EXIT POSTPONE THEN ; IMMEDIATE\n"

 ": PRIM? 0< ['] DUP 0< = ;\n"

 ": ?CONST\n" // ( cfa -- cfa flag )
 "DUP PRIM? IF 0 BREAK\n"
 "DUP @ DOCONST = ;\n"

 ": ?VARIABLE\n" // ( cfa -- cfa flag )
 "DUP PRIM? IF 0 BREAK\n"
 "DUP @ DOVAR = ;\n"
	": FQUIT  BEGIN REFILL WHILE CURSTR 1+!\n"
	"  INTERPRET  REPEAT ;\n"

	": LALIGNED  3 + 3 ANDC ;\n"

	" 255 CONSTANT TC/L\n"

 ": INCLUDE-FILE\n" // ( fid --- )
// Read lines from the file identified by fid and interpret them.
// INCLUDE and EVALUATE nest in arbitrary order.
	"SOURCE-ID >R >IN @ >R LASTIN @ >R CURSTR @ >R CURSTR 0!\n"
	"SOURCE 2>R\n"
	" TC/L ALLOCATE THROW TC/L SOURCE!\n"
	"TO SOURCE-ID\n"
	"['] FQUIT CATCH SAVEERR\n"
	"TIB FREE DROP\n"
	"2R> SOURCE!\n"

	"R> CURSTR ! R> LASTIN ! R> >IN ! R> TO SOURCE-ID\n"
	"THROW ;\n"

	": FREFILL0\n" // (  -- flag )
	"  TIB TC/L SOURCE-ID READ-LINE THROW\n"
	"  SWAP  #TIB !  0 >IN ! CURSTR 1+!\n"
	"  0 SOURCE + C! ;\n"
	"' FREFILL0 TO FREFILL\n"

  "444 CONSTANT  CFNAME_SIZE\n"
  "CREATE CURFILENAME  CFNAME_SIZE 255 + 1+ ALLOT\n"
  "CURFILENAME  CFNAME_SIZE 255 + 1+  ERASE\n"

  ": CFNAME-SET\n" // ( adr len -- )
  "DUP 1+ >R  CURFILENAME CURFILENAME R@ + CFNAME_SIZE R> - CMOVE>\n"
  "CURFILENAME $! ;\n"

  ": CFNAME-FREE\n" //  ( -- )
  "CURFILENAME COUNT + CURFILENAME\n"
  "CFNAME_SIZE CURFILENAME C@ - 255 +  CMOVE ;\n"

 ": INCLUDED\n" // ( c-addr u ---- )
 "2DUP CFNAME-SET\n"
 "R/O OPEN-FILE THROW\n"
 "DUP >R ['] INCLUDE-FILE CATCH\n"
 "DUP IF cr .\" in <\" CURFILENAME COUNT TYPE .\" >\" CURSTR @ . THEN  CFNAME-FREE\n"
 "R> CLOSE-FILE DROP THROW ;\n"

 ": ARGDO  ARGC @ 1- IF\n"
 " ARGC @ 1 DO  ARGVT @ I CELLS + @ ZCOUNT EVALUATE  LOOP\n"
// " ARGC @ 1 DO  ARGVT @ I DUP H. CELLS + @ ZCOUNT TYPE LOOP\n"
 "THEN ; ARGDO\n"

  "S\" autoexec.4th\" INCLUDED\n"

;

void  Key()
{   uint8_t cc[9];
	int KN ;
    if (KN=sys_read(STDIN, &cc, 1) != 1)  printf("KN=%d",KN);
  *--Stack= Tos;
  Tos= (Cell) cc[0] ;

} pp(Key)

void  InitStringSet()
{ 	tib[1]=(uint64_t)initScript;
	ntib=strlen(initScript);
    *v2in = 0;
} pp(InitStringSet)

#define SysConstant(NAME) Constant( #NAME, NAME )

void  MakeImag(void)
{   FthItem("NOOP",~pNoop );
    FthItem("+",~pAdd );
    FthItem("-",~pSub );
    FthItem("D+",~pDAdd );
    FthItem("1+",~pi1Add );
    FthItem("1-",~pi1Sub );
    FthItem("2+",~pi2Add );
    FthItem("2-",~pi2Sub );
    FthItem("INVERT",~pInvert);
    FthItem("NEGATE",~pNegate);
    FthItem("DNEGATE",~pDNegate);
    FthItem("DABS",~pDAbs);
    FthItem("*",~pMul);
    FthItem("/",~pDiv);
    FthItem("2*",~pi2Mul);
    FthItem("2/",~pi2Div);
    FthItem("MOD",~pMod);
    FthItem("U*",~pUMul);
    FthItem("U/",~pUDiv);
    FthItem("UM*",~pUMMul);
    FthItem("UM/MOD",~pUMMOD);
    FthItem("/MOD",~pDIVMOD);
    FthItem("AND",~pAnd);
    FthItem("ANDC",~pAndC);
    FthItem("OR",~pOr);
    FthItem("XOR",~pXor);
    FthItem("ARSHIFT",~pARshift);
    FthItem("RSHIFT",~pRshift);
    FthItem("LSHIFT",~pLshift);
    FthItem("DUP",~pDup );
    FthItem("CS-DUP",~pDup );
    FthItem("?DUP",~pQDup );
    FthItem("OVER",~pOver );
    FthItem("CS-OVER",~pOver );
    FthItem("TUCK",~pTuck );
    FthItem("PICK",~pPick );
    FthItem("CS-PICK",~pPick );
    FthItem("SWAP",~pSwap );
    FthItem("CS-SWAP",~pSwap );
    FthItem("2SWAP",~pi2Swap );
    FthItem("ROT",~pRot );
    FthItem("DROP",~pDrop );
    FthItem("NIP",~pNip );
    FthItem("2DROP",~pi2drop );
    FthItem("2DUP",~pi2dup );
    FthItem("2OVER",~pi2over);
    FthItem(".",~pDot);
    FthItem("U.",~pUDot);
    FthItem("H.",~pHDot);
    FthItem("CATCH",PCatch);
    FthItem("THROW",~pFThrow);
    FthItem("[",~pIMode); Immediate();
    FthItem("]",~pCMode);
    FthItem("@",~pLoad);
    FthItem("C@",~pCLoad);
    FthItem("C!",~pCStore);
    FthItem("C+!",~pCAddStore);
    FthItem("C!A",~pCStoreA);
    FthItem("W@",~pWLoad);
    FthItem("W!",~pWStore);
    FthItem("2!",~pi2Store);
    FthItem("2@",~pi2Load);
    FthItem("COUNT",~pCount);
    FthItem("!",~pStore);
    FthItem("+!",~pAddStore);
    FthItem("1+!",~pIncr);
    FthItem("0!",~pOff);
    FthItem("OFF",~pOff);
    FthItem("ON",~pOn);
    FthItem("=",~pEqual);
    FthItem("<>",~pNEqual);
    FthItem("0<",~pZLess);
    FthItem("0=",~pZEqual);
    FthItem("0<>",~pZNEqual);
    FthItem("D0=",~pDZEqual);
    FthItem("<",~pLess);
    FthItem(">",~pGreat);
    FthItem("U<",~pULess);
    FthItem("U>",~pUGreat);
    FthItem("MAX",~pMax);
    FthItem("MIN",~pMin);
    FthItem("0MAX",~pi0Max);
    FthItem(">R",~pToR);

    FthItem("R>",~pFromR);
    FthItem("RDROP",~pRDrop);
    FthItem("R@",~pRLoad);
    FthItem("2>R",~pi2ToR);
    FthItem("2R>",~pi2FromR);
    FthItem("2R@",~pi2RLoad);
    FthItem("RP@",~pRLoad);
    FthItem("RP@",~pRPGet);
    FthItem("SP@",~pSPGet);
    FthItem("RP!",~pRPSet);
    FthItem("SP!",~pSPSet);
    FthItem(",",~pCompile);
    FthItem("ALLOT",~pAllot);
    FthItem("$,",~pStrCmp);
    FthItem("<$>",~pStr);
    FthItem("EXECUTE",~pExecute);
    FthItem("SMUDGE",~pSmudge);
    FthItem("TYPE",~pType);
    FthItem("ZTYPE",~pZType);
    FthItem("CR",~pCr);
    FthItem("SPACE",~pSpace);
    FthItem("EMIT",~pEmit);
    FthItem(">IN",Pi2in);
    FthItem("PARSE-NAME",~pParseName);
    FthItem("PARSE",~pParse);
    FthItem("SHEADER",~pSHeader);
    FthItem("BUILD",~pBuild);
    FthItem("SFIND",~pSFind);
    FthItem("SEARCH-WORDLIST",~pSearchWordList);
    FthItem("UCOMPARE",~pUCompare);
    FthItem("FILL",~pFill);
    FthItem("CMOVE",~pCmove);
    FthItem("CMOVE>",~pCmove_up);
    FthItem("ZCOUNT",~pZCount);

    FthItem("KEY",~pKey);
    sCell PKey = Header("KEY");  Co2(~pDoDefer,~pKey);

    FthItem("IMMEDIATE",~pImmediate);
    FthItem(":",~pColon);
    FthItem(";",~pSemicolon);   Immediate();
    FthItem("IF",~pIf);         Immediate();
    FthItem("ELSE",~pElse);     Immediate();
    FthItem("THEN",~pThen);     Immediate();
    FthItem("BEGIN",~pBegin);   Immediate();
    FthItem("UNTIL",~pUntil);   Immediate();
    FthItem("AGAIN",~pAgain);   Immediate();
    FthItem("WHILE",~pWhile);   Immediate();
    FthItem("REPEAT",~pRepeat); Immediate();

    sCell PTrue = Constant("TRUE",-1);

    FthItem("EXIT",~pExit );
    Constant("STATE",(sCell) &State );
    FthItem("?STATE",~pStateQ);

    Constant("DOVAR",~pDoVar );
    Constant("DOCONST",~pDoConst );
    Constant("DODEFER",~pDoDefer );
    Constant("DP", (sCell)&here );
    Constant("LAST", (sCell)&Last );
    Constant("LASTCFA", (sCell)&LastCFA );
    VVariable("WARNING",-1);
    FthItem("HERE",~pHere);
    Constant("BL",(sCell)' ' );
    sCell PCell = Constant("CELL",sizeof(Cell) );

    FthItem("NAME>",~pFromName);
    Constant("BASE",(sCell)&numericBase);

    Header("'");   Co5(~pParseName,~pSFind,~pZEqual,~pFThrow,~pExit);

    Constant("STATE",(sCell) &State );
    sCell PHi = Header("HI"); Tp("Hello!!!"); Co(~pExit);
    sCell PLastin = Constant("LASTIN", (sCell)&Lastin );
    sCell PSaveErrQ = Constant("SAVEERR?", (sCell)&SaveErrQ );

    FthItem("SAVEERR0",~pSaveErr0);
    sCell PSaveErr = Header("SAVEERR");  Co2(~pDoDefer,~pSaveErr0);
    FthItem("PRINTERR0",~pPrintErr0);

    sCell PContext = Constant("CONTEXT",(sCell) &Context );
    Constant("CURRENT",(sCell) &Current );
    Constant("IMAGE-BEGIN",(sCell)HereArea );
    Constant("FORTH-WORDLIST",(sCell) &ForthWordlist );
    Constant("CONTEXT-SIZE",ContextSize );
    sCell PSP0 = VVariable("SP0",(sCell) &StackArea[STACK_SIZE-9] );

    FthItem("R/O",~preadOnly);
    FthItem("R/W",~preadWrite);
    FthItem("W/O",~pwriteOnly);

    FthItem("OPEN-FILE",~popenFile);
    FthItem("READ-FILE",~preadFile);
    FthItem("READ-LINE",~preadLine);
    FthItem("WRITE-FILE",~pwriteFile);

    FthItem("CLOSE-FILE",~pcloseFile);

    FthItem("TIB",Ptib);
    sCell PATib = Constant("ATIB",(sCell)&atib);
    sCell Pntib = Constant("#TIB",(sCell)&ntib);

    FthItem("SOURCE",~pSource);
    FthItem("SOURCE!",~pSourceSet);
    FthItem("SOURCE-ID",PSourceId);

    FthItem("ALLOCATE",~pAllocate);
    FthItem("FREE",~pFree);

    Constant("YDP", (sCell)&YDP);
    Constant("YDP0", (sCell)&YDP0);
    FthItem("YDP_FL",~pYDPFL);

    Constant("ARGVT",(sCell)&ARGVT);
    Constant("ARGC",(sCell)&ARGC);

    SysConstant(SYSCALL_DEBUGLOG);
    SysConstant(SYSCALL_MMAP);
    SysConstant(SYSCALL_OPENAT);
    SysConstant(SYSCALL_READ);
    SysConstant(SYSCALL_WRITE);
    SysConstant(SYSCALL_SEEK);
    SysConstant(SYSCALL_CLOSE);
    SysConstant(SYSCALL_SET_FS_BASE);
    SysConstant(SYSCALL_IOCTL);
    SysConstant(SYSCALL_GETPID);
    SysConstant(SYSCALL_CHDIR);
    SysConstant(SYSCALL_MKDIRAT);
    SysConstant(SYSCALL_SOCKET);
    SysConstant(SYSCALL_BIND);
    SysConstant(SYSCALL_FORK);
    SysConstant(SYSCALL_EXECVE);
    SysConstant(SYSCALL_FACCESSAT);
    SysConstant(SYSCALL_FSTATAT);
    SysConstant(SYSCALL_FSTAT);
    SysConstant(SYSCALL_GETPPID);
    SysConstant(SYSCALL_FCNTL);
    SysConstant(SYSCALL_DUP3);
    SysConstant(SYSCALL_WAITPID);
    SysConstant(SYSCALL_EXIT);
    SysConstant(SYSCALL_READDIR);
    SysConstant(SYSCALL_MUNMAP);
    SysConstant(SYSCALL_GETCWD);
    SysConstant(SYSCALL_GETCLOCK);
    SysConstant(SYSCALL_READLINK);
    SysConstant(SYSCALL_GETRUSAGE);
    SysConstant(SYSCALL_GETRLIMIT);
    SysConstant(SYSCALL_UNAME);
    SysConstant(SYSCALL_FUTEX_WAIT);
    SysConstant(SYSCALL_FUTEX_WAKE);
    SysConstant(SYSCALL_MEMINFO);
    SysConstant(SYSCALL_PIPE);
    SysConstant(SYSCALL_UNLINK);
    SysConstant(SYSCALL_CHMOD);
    SysConstant(SYSCALL_RUNCMD);
    SysConstant(SYSCALL_GETENTROPY);
    SysConstant(SYSCALL_SIGPROCMASK);
    SysConstant(SYSCALL_SIGACTION);

    sCell PErrDO1 = Header("ERROR_DO1"); Co3(PSaveErr,~pPrintErr0,~pExit);
    sCell PErrDO = Header("ERROR_DO");  Co2(~pDoDefer,PErrDO1);

    sCell PAccept = Header("ACCEPT");  Co2(~pDoDefer,~pAccept);
    sCell PQuery = Header("QUERY");
    Co4(Ptib,~pLit_,256,PAccept);
    Co5(Pntib,~pStore,Pi2in,~pOff,~pExit);
    FthItem("BYE",~pBye);

    sCell PLitC = Header("LIT,");  Co2(~pDoDefer,~pLitCo);
    sCell PPre = Header("<PRE>");  Co2(~pDoDefer,~pNoop);
    sCell PFileRefill = Header("FREFILL");  Co2(~pDoDefer,~pNoop);
    sCell PQStack = Header("?STACK");  Co2(~pDoDefer,~pNoop);

    sCell PRefill = Header("REFILL");
	Co(PSourceId);
    If();   Co2(PFileRefill,~pDup);  If(); Co(PPre); Then();
    Else(); Co2(PQuery,PTrue);
    Then(); Co(~pExit);

    FthItem("SNUMBER0",~pSNumber0);

    sCell PSNumber = Header("SNUMBER");  Co2(~pDoDefer,~pSNumber0 );

    sCell PQSLiteral0 = Header("?SLITERAL0");
	Co(PSNumber);
	If(); Lit(-13); Co(~pFThrow);
	Else(); Co(~pStateQ); If(); Co(PLitC); Then();
	Then();
    Co(~pExit);

    sCell PQSLiteral = Header("?SLITERAL");
    Co2(~pDoDefer,PQSLiteral0);

    sCell PInterpret1 = Header("INTERPRET1");
    Begin();
        Co6(Pi2in,~pLoad,PLastin,~pStore,PSaveErrQ,~pOn);
        Co2(~pParseName,~pDup);
    While();  Co2(~pSFind,~pQDup);
        If();
            Co2(~pStateQ,~pEqual);
            If();   Co(~pCompile );
            Else(); Co(~pExecute );
            Then();
        Else(); Co(PQSLiteral);
        Then(); Co(PQStack);
    Repeat();


    Co2(~pi2drop,~pExit);

    sCell PInterpret = Header("INTERPRET");
    Co2(~pDoDefer,PInterpret1 );
    sCell PQuit = Header("QUIT");
    Begin();	Co(PRefill); 
    While();	Co(PInterpret);	Tp(" ok\n\033[36m> \033[0m");
    Repeat();	Co(~pExit);

    sCell PWords = Header("WORDS");
    Co3(PContext,~pLoad,~pLoad);
    Begin(); Co(~pDup);
    While(); Co7(~pDup,~pCount,~pType,~pSpace,PCell,~pSub,~pLoad );
    Repeat(); Co2(~pDrop,~pExit );

    ip = here;  // SYS START

    Co(~pInitStringSet);
    Co5(~pIMode,~pLit_,PInterpret,PCatch,~pQDup );
    If(); Co5(PErrDO,PSP0,~pLoad,~pSPSet,~pCr ) ;
    Then();

    Begin();
	Co4(PATib,~pLit_,(sCell)&tib[1],~pStore);
        Co5(~pIMode,~pLit_,PQuit,PCatch,PErrDO);
        Co4(PSP0,~pLoad,~pSPSet,~pCr ) ;
    Again();

}


int main(int argc, char *argv[])
{
	tib[0]=~pDoConst;

//	HereArea = sys_malloc(sizeof(sCell)*HERE_SIZE1);
	StackArea = sys_malloc(sizeof(sCell)*STACK_SIZE);
	RStackArea = sys_malloc(sizeof(sCell)*RSTACK_SIZE);

        if((pNoop<0)^((sCell)HereArea<0))
        {fprintf(STDERR, "The addresses do not have a single sign.\n");
	sys_exit(1);
	}

	i2in[0]=~pDoVar;
	SourceId[0]=~pDoConst;
	Catch[0] = ~pto_catch;
	Catch[1] = ~pfrom_catch;
	memset(input_buffer, 0, 256);


	here = HereArea ;
	Stack = &StackArea[STACK_SIZE-8] ;
	rStack = &RStackArea[RSTACK_SIZE-8] ;

        ForthWordlist[0] = 0;
        ForthWordlist[1] = 0;
        ForthWordlist[2] = 0;

	Context[0] = ForthWordlist;
	Context[1] = 0;
	Current[0] = ForthWordlist;
	ireg = ~(sCell)MakeImag;

    ARGVT=argv;
    ARGC= argc;

  if (argc < 2)
  { printf("WORDS - list of words\n");
  }
  if(pNoop>0){
//      printf("positiv\n"); // addresses area
	
      while (1)
      {   do{
              ((proc) (~ireg) )();
              ireg = *ip++;
          }while ( ireg<0);
          do{
              *--rStack = (sCell) ip;  ip =  (sCell *) ireg;
              ireg = *ip++;
          }while ( ireg>0);
      }
  }
  else{
//      printf("negative\n"); // addresses area
      while (1)
        {   do{
                ((proc) (~ireg) )();
                ireg = *ip++;
            }while ( ireg>0);
            do{
                *--rStack = (sCell) ip;  ip =  (sCell *) ireg;
                ireg = *ip++;
            }while ( ireg<0);
        }
    }
    return 0;
}
