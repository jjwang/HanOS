
: [ELSE]
    1
    BEGIN
      PARSE-NAME DUP
      IF  
         2DUP   S" \"  UCOMPARE 0=   IF 2DROP POSTPONE \	ELSE 
         2DUP 3 UMIN  S" [IF"  \ все слова с префиксом "[IF"
                        UCOMPARE 0= IF 2DROP 1+                 ELSE 
         2DUP S" [ELSE]" UCOMPARE 0= IF 2DROP 1- DUP  IF 1+ THEN ELSE 
              S" [THEN]" UCOMPARE 0= IF       1-                 THEN
                                    THEN THEN THEN
      ELSE 2DROP REFILL  AND \   SOURCE TYPE
      THEN DUP 0=
    UNTIL  DROP ;  IMMEDIATE

: [IF] 0= IF POSTPONE [ELSE] THEN ;  IMMEDIATE

: [THEN] ;  IMMEDIATE
