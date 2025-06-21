
\- PAD  HERE  $200 ALLOT $100 + CONSTANT PAD

VARIABLE  HLD

: HOLD ( char -- ) \ 94
  HLD @ 1- DUP HLD ! C!
;

: <# ( -- ) \ 94
  PAD 1- HLD !
  0 PAD 1- C!
;

: # ( ud1 -- ud2 ) \ 94
  0 BASE @ UM/MOD >R BASE @ UM/MOD R>
  ROT DUP 10 < 0= IF 7 + THEN 48 + 
  HOLD
;

: #S ( ud1 -- ud2 ) \ 94
  BEGIN
    # 2DUP D0=
  UNTIL
;


: #> ( xd -- c-addr u ) \ 94
  2DROP  HLD @  PAD OVER - 1-
;

: SIGN ( n -- ) \ 94
  0< IF [CHAR] - HOLD THEN
;

: (D.) ( d -- addr len )
  DUP >R DABS <# #S R> SIGN #>
;

: HOLDS ( addr u -- ) \ from eserv src
  TUCK + SWAP 0 ?DO DUP I - 1- C@ HOLD ( /CHAR +LOOP FIXME) LOOP DROP
;

: SPACES       ( N  -- )
    0MAX 80 MIN  0 ?DO SPACE LOOP 
;

: D. ( d -- ) \ 94 DOUBLE
  (D.) TYPE SPACE ;

