 .( ./vt100.fs ) CR

DECIMAL
\ markup language for terminal control codes
\ pn is a "parameter number" defined in VT100.FS

: pn    base @ swap decimal 0 u.r base ! ;
: ;pn   [char] ; emit pn ;
: ESC[  27 emit [char] [ emit ;


\ : <ESC>   ( -- ) 27 EMIT ;
: <UP>    ( n -- ) ESC[ pn ." A" ;
: <DOWN>  ( n -- ) ESC[ pn ." B" ;
: <RIGHT> ( n -- ) ESC[ pn ." C" ;
: <BACK>  ( n -- ) ESC[ pn ." D" ;
: <HOME>  ( -- )   ESC[ ." H"  ;

0 CONSTANT RESET
1 CONSTANT BRIGHT
2 CONSTANT DIM
4 CONSTANT UNDERSCORE
5 CONSTANT BLINK
7 CONSTANT REVERSE
8 CONSTANT HIDDEN

DECIMAL
\ colour modifiers
: FG>  ( n -- n') 30 + ;
: BG>  ( n -- n') 40 + ;

\  Colours are used with FG>  BG>

0 CONSTANT <BLK
1 CONSTANT <RED
2 CONSTANT <GRN
3 CONSTANT <YEL
4 CONSTANT <BLU
5 CONSTANT <MAG
6 CONSTANT <CYN
7 CONSTANT <WHT

: <COLOR>   ( fg bg -- ) ESC[ pn ;pn ." m"  ;
: <ATTRIB>  ( n -- )     ESC[ pn ." ;m"   ;

\ <RED FG>  <GRN BG> <COLOR> 
