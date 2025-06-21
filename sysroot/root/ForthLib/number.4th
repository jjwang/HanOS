
: DIGIT ( C, N1 -> N2, TF / FF ) 
\ N2 - 
\ 
   >R
   [CHAR] 0 - 10 OVER U<
   IF 
      DUP [CHAR] A [CHAR] 0 -     < IF  RDROP DROP 0 EXIT      THEN
      DUP [CHAR] a [CHAR] 0 -  1- > IF [CHAR] a  [CHAR] A - -  THEN
          [CHAR] A [CHAR] 0 - 10 - -
   THEN R> OVER U> DUP 0= IF NIP THEN ;


: >NUMBER ( ud1 c-addr1 u1 -- ud2 c-addr2 u2 ) \ 94
  BEGIN
    DUP
  WHILE
    >R
    DUP >R
    C@ BASE @ DIGIT 0=     \ ud n flag
    IF R> R> EXIT THEN     \ ud n  ( ud = udh udl )
    SWAP BASE @ UM* DROP   \ udl n udh*base
    ROT BASE @ UM* D+      \ (n udh*base)+(udl*baseD)
    R> 1+ R> 1-
  REPEAT
;
\ S" 
