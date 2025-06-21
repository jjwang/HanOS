 HI .(  from autoexec.4th ) CR

: REQUIRED ( waddr wu laddr lu -- )
  2SWAP  SFIND
  IF DROP 2DROP EXIT
  ELSE 2DROP  INCLUDED THEN ;

: REQUIRE ( "word" "libpath" -- ) PARSE-NAME PARSE-NAME  REQUIRED ;

\- ->DEFER : ->DEFER ( cfa <name> -- )  HEADER DODEFER , , ;
\- DEFER : DEFER ( <name> -- ) ['] ABORT ->DEFER ;

\- VECT : VECT DEFER ;
\- FALSE 0 CONSTANT FALSE
\- TRUE -1 CONSTANT TRUE
\-  MIN :  MIN 2DUP  < IF DROP EXIT THEN NIP ;
\-  MAX :  MAX 2DUP  < IF NIP EXIT THEN DROP ;
\- UMIN : UMIN 2DUP U< IF DROP EXIT THEN NIP ;
\- UMAX : UMAX 2DUP U< IF NIP EXIT THEN DROP ;
\- ABS : ABS DUP 0< IF NEGATE THEN ;
\- -ROT : -ROT ROT ROT ;
\- >IN_WORD LASTIN CONSTANT >IN_WORD
\- CHAR+ : CHAR+ 1+ ;
\- CHARS : CHARS ;
\- CDR : CDR CELL- @ ;
\- U>D : U>D 0 ;
\- WITHIN : WITHIN ( n lo hi+1 -- flag ) OVER -  >R  -  R> U< ;
\- PERFORM : PERFORM @ EXECUTE ;
\- C, : C, ( word -- )  HERE 1 ALLOT C! ;
\- W, : W, ( word -- )  HERE 2 ALLOT W! ;

REQUIRE [IF] ForthLib/CompIF4.4th

: [IFDEF]
    POSTPONE [UNDEFINED]
    IF POSTPONE [ELSE] THEN
; IMMEDIATE

: [IFNDEF]
    POSTPONE [DEFINED]
    IF POSTPONE [ELSE] THEN
; IMMEDIATE


[IFNDEF] \EOF
: \EOF  ( -- )
  BEGIN REFILL 0= UNTIL
  POSTPONE \
;
[THEN]

[IFNDEF] DEPTH
: DEPTH ( -- +n ) \ 94
\ +n - число одинарных ячеек, находящихся на стеке данных перед
\ тем как туда было помещено +n.
  SP@ SP0 @ - NEGATE CELL U/
;
[THEN]


[IFNDEF] .SN
: .SN ( n --)
\ Распечатать n верхних элементов стека
   >R BEGIN
         R@
      WHILE
        SP@ R@ 1- CELLS + @ H.
        R> 1- >R
      REPEAT RDROP
;
[THEN]
\- .S : .S ( -- )   DEPTH .SN ;

[IFNDEF] CMOVE
: CMOVE ( c-addr1 c-addr2 u --- )
\ Copy u bytes starting at c-addr1 to c-addr2, proceeding in ascending
\ order.
   DUP IF >R 
   BEGIN
    OVER C@ SWAP C!A 1+ SWAP 1+ SWAP 
    R> 1- DUP >R 0=
   UNTIL
   R>
   THEN
   DROP DROP DROP
;
[THEN]

[IFNDEF] CMOVE>
: CMOVE> ( c-addr1 c-addr2 u --- )
\ Copy a block of u bytes starting at c-addr1 to c-addr2, proceeding in
\ descending order.
   DUP IF >R R@ + 1- SWAP R@ + 1- SWAP 
   BEGIN
    OVER C@ SWAP C!A 1- SWAP 1- SWAP
    R> 1- DUP >R 0=     
   UNTIL
   R>
   THEN
   2DROP DROP 
;
[THEN]

[IFNDEF] MOVE

: MOVE ( addr1 addr2 u -- ) \ 94
\ Если u больше нуля, копировать содержимое u байт из addr1 в addr2.
\ После MOVE в u байтах по адресу addr2 содержится в точности то же,
\ что было в u байтах по адресу addr1 до копирования.
  >R 2DUP SWAP R@ + U< \ назначение попадает в диапазон источника или левее
  IF 2DUP U<           \ И НЕ левее
     IF R> CMOVE> ELSE R> CMOVE THEN
  ELSE R> CMOVE THEN ;
[THEN]

: MOVE ( addr1 addr2 u -- ) \ 94	
  >R 2DUP SWAP R@ + U<
  IF 2DUP U<
     IF R> CMOVE> ELSE R> CMOVE THEN
  ELSE R> CMOVE THEN ;

\- MAX$@ 255 CONSTANT MAX$@

: "CLIP"        ( a1 n1 -- a1 n1' )   \ clip a string to between 0 and MAXCOUNTED
                0 MAX MAX$@ AND ( UMIN ) ;

: $!         ( addr len dest -- )
        SWAP "CLIP" SWAP
	2DUP C! CHAR+ SWAP CMOVE ;


: $+!       ( addr len dest -- ) \ append string addr,len to counted
                                     \ string dest
                >R "CLIP" MAX$@  R@ C@ -  MIN R>
                                        \ clip total to MAXCOUNTED string
	2DUP 2>R
	COUNT  + SWAP CMOVE
	2R> C+! ;

: $C+!       ( c1 a1 -- )    \ append char c1 to the counted string at a1
	DUP 1+! COUNT + 1- C! ;

: C+PLACE $C+! ;

: +NULL         ( a1 -- )       \ aMINppend a NULL just beyond the counted chars
                COUNT + 0 SWAP C! ;

\- \S : \S \EOF ; IMMEDIATE

: WL_NEAR_NFA_N ( addr nfa - addr nfa | addr 0 )
\ cr ." ~~~" over h. cr
   BEGIN 2DUP DUP IF NAME> THEN U<
   WHILE \ ." <" dup NAME> h. dup count type bl emit ." >" CR
  CDR
   REPEAT
;


: N_UMAX ( nfa nfa1 -- nfa|nfa1 )
 OVER DUP IF NAME> THEN
 OVER DUP IF NAME> THEN U< IF NIP EXIT THEN DROP ;

: WL_NEAR_NFA_M (  addr wid - nfa2 addr | 0 addr )
   0 -ROT
   @  
   BEGIN  DUP
   WHILE  WL_NEAR_NFA_N  \  nfa addr nfa1
       SWAP >R 
       DUP  >R  N_UMAX 
       R>  DUP  IF CDR THEN 
       R>  SWAP

   REPEAT DROP
;

: NEAR_NFA ( addr - nfa addr | 0 addr )
   0 SWAP 
   VOC-LIST
   BEGIN  @ DUP
   WHILE DUP  >R
  CELL-
  WL_NEAR_NFA_M
   >R  N_UMAX  R>  R>
   REPEAT DROP
;

: >NAME    ( CFA -- NFA  )
 NEAR_NFA DROP ;

: SMUDG LAST @ CURRENT @ ! ;

:  VOCS. 
 VOC-LIST @
 BEGIN ?DUP
 WHILE DUP CELL- CELL- VOC-NAME. @
 REPEAT
;

: (ABORT'')	ROT IF TYPE -2 THROW THEN 2DROP ;

: ABORT"	[COMPILE] S" ['] (ABORT'') COMPILE, ; IMMEDIATE

: ?ERROR ( F, N -> )  SWAP IF THROW ELSE DROP THEN ;

CREATE ERRTIB  0 , TC/L ALLOT
VARIABLE ER>IN
VARIABLE ERR&S-ID
VARIABLE ERR-LINE
CREATE ERRFILENAME 0 ,  255 ALLOT

: SAVEERR1
	DUP SAVEERR? @ AND
	IF	SOURCE ERRTIB $! LASTIN @ ER>IN !
		SOURCE-ID ERR&S-ID ! CURSTR @ ERR-LINE ! SAVEERR? OFF
		CURFILENAME COUNT ERRFILENAME $!
	THEN
;
' SAVEERR1 TO SAVEERR

: ERROR_DO2
	SAVEERR
	CR ERRTIB COUNT TYPE CR
\ ER>IN @ BEGIN SPACE 1- DUP 0 MAX 0= UNTIL
	ERRTIB 1+ ER>IN @ 63 ( $3F) AND 0
   ?DO COUNT 9 = IF 9 EMIT ELSE SPACE THEN  LOOP DROP
 ." ^" \ DROP
	CR ." Err=" . 
	CR  SP0 @ SP!   STATE 0!
;

' ERROR_DO2 TO ERROR_DO

REQUIRE >NUMBER ForthLib/number.4th

[IFNDEF] (*  \ *)
: (*  ( -- )
  BEGIN
    PARSE-NAME DUP 0=
    IF  NIP  REFILL   0= IF DROP TRUE THEN
    ELSE  S" *)" UCOMPARE 0= THEN
  UNTIL
; IMMEDIATE
[THEN]

[IFNDEF] ((  \ ))
: ((  ( -- )
  BEGIN
    PARSE-NAME DUP 0=
    IF  NIP  REFILL   0= IF DROP TRUE THEN
    ELSE  S" ))" UCOMPARE 0= THEN
  UNTIL
; IMMEDIATE
[THEN]

\ REQUIRE MODULE:
FLOAD ForthLib/spf_modules.4th

FLOAD ForthLib/fpcnum.4th
FLOAD ForthLib/dpoint.4th 

[IFNDEF] FILL
: FILL ( c-addr u c ---)
\ Fill a block of u bytes starting at c-addr with character c.
  OVER IF >R 
  BEGIN
   R@ ROT C!A 1 + SWAP   
   1 - DUP 0= 
  UNTIL
  R>
  THEN
  DROP DROP DROP
;     
[THEN]

\- CURSTR VARIABLE CURSTR

\- CS-DUP : CS-DUP 0 CS-PICK ;

\- CS-SWAP : CS-SWAP 1 CS-ROLL ;

\- M_WL : M_WL  CS-DUP POSTPONE WHILE ; IMMEDIATE

\- LAST-NON 0 VALUE LAST-NON


\- :NONAME : :NONAME ( C: -- colon-sys ) ( S: -- xt ) \ 94 CORE EXT
\- :NONAME  HERE DUP TO LAST-NON [COMPILE] ] ;
\- S", : S", $, ;
\- S>D : S>D  DUP 0< ;

\- #DEFINE : #DEFINE  HEADER DOCONST COMPILE, INTERPRET , ; 

[IFNDEF] U.R
: U.R ( u n -- ) \ 94 CORE EXT
  >R  U>D <# #S #>
  R> OVER - 0 MAX SPACES TYPE ;
[THEN]

: >PRT
  DUP BL U< IF DROP [CHAR] . THEN
;

: .0
  >R 0 <#  #S  #> R> OVER - 0 MAX
    BEGIN  DUP 0>
    WHILE 1- [CHAR] 0 EMIT
    REPEAT DROP
    TYPE 
;

: PTYPE
    BEGIN DUP 0>
    WHILE 1- SWAP COUNT >PRT EMIT SWAP
    REPEAT 2DROP
;

: DUMP ( addr u -- ) \ 94 TOOLS
  DUP 0= IF 2DROP EXIT THEN
  BASE @ >R HEX
  15 + 16 U/ 0 DO
    CR DUP 4 .0 SPACE
    SPACE DUP 16 0
      DO  I 3 AND 0= IF SPACE THEN
         DUP       C@
\	0xFF AND
 2 .0 SPACE  1+
      LOOP
       SWAP  16  PTYPE
  LOOP DROP R> BASE !
;

FLOAD ForthLib/vt100.fs

: COLOR_ERROR_DO
<RED FG>  <BLK BG> <COLOR> 
 [ ' ERROR_DO DEFER@ COMPILE, ]
<GRN FG>  <BLK BG> <COLOR> 
;

' COLOR_ERROR_DO TO ERROR_DO

:NONAME
." WORDS -  List the definition names" CR
." BYE  - exit from FORTH" CR CR
; ->DEFER HELP

HELP

