PATH=/data/bin:/bin:/usr/bin
export PATH
TERM=hanos
export TERM
COLORTERM=truecolor
export COLORTERM
PS1='\033[1;32m\u@\h\033[0m:\033[1;34m\w\033[0m \$ '
HISTCONTROL=ignoredups
HISTSIZE=-1
HISTFILESIZE=-1
export LS_COLORS='di=01;34:ln=01;36:pi=33:so=01;35:bd=33;01:cd=33;01:or=40;31;01:ex=01;32'
alias ls='ls --color=auto'
alias grep='grep --color=auto'
