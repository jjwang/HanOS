\ $Id: spf_modules.f,v 1.7 2006/12/04 21:15:59 ygreks Exp $

\  Working with forth modules
\   Copyright [C] 2000 D.Yakimov day@forth.org.ru


: MODULE: ( "name" -- old-current )
  >IN @ 
  ['] ' CATCH
  IF >IN ! VOCABULARY LATEST NAME> ELSE NIP THEN
  GET-CURRENT SWAP ALSO EXECUTE DEFINITIONS ;

: EXPORT ( old-current -- old-current )
\ export some module definitions
  DUP SET-CURRENT 
;

: ;MODULE ( old-current -- )
\ finish the module
   SET-CURRENT PREVIOUS
;

: {{ ( "name" -- )
        DEPTH >R
        ALSO ' EXECUTE
        DEPTH R> <>             IF      \ wid on the stack?
             CONTEXT !          THEN
; IMMEDIATE

: }}
   PREVIOUS
; IMMEDIATE