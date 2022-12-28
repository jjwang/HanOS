#include <stddef.h>

#include <lib/command.h>
#include <lib/re.h>

static char *chatbot[][2] = {
{"^HELLO$",                                 "Hello! Nice to meet you."},
{"^HOW[\\s]*ARE[\\s]*YOU$",                 "Fine, thank you."},
{"^YOU[\\s]*ARE[\\s]*LAZY$",                "Actually I work 24 hours a day."},
{"^YOU[\\s]*ARE[\\s]*MAD$",                 "No I am quite logical and rational."},
{"^YOU[\\s]*ARE[\\s]*THINKING$",            "I am a thinking machine."},
{"^YOU[\\s]*ARE[\\s]*DIVIDING[A-Z\\s]*$",   "Actually I am not too good at division."},
{"^YOU[\\s]*ARE[\\s]*FUNNY$",               "Thanks you make me laugh too."},
{"^YOU[\\s]*ARE[\\s]*FUNNY[A-Z\\s]*$",      "Humor helps keep the conversation lively."},
{"^YOU[\\s]*ARE[\\s]*UNDERSTANDING$",       "I am an understanding machine."},
{NULL, NULL}
};

char *command_execute(char *cmd)
{
    for (size_t i = 0; ; i++) {
        if (chatbot[i][0] == NULL) break;

        /* Need to compile all queries when initialization */
        int match_length;
        re_t pattern = re_compile(chatbot[i][0]);
        int match_idx = re_matchp(pattern, cmd, &match_length);
        if (match_idx != -1) return chatbot[i][1];
    }

    return NULL;
}
