#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

typedef enum state {
    Undefined,
    // TODO: Add additional states as necessary
    AUTHORIZATION,
    TRANSACTION,
    UPDATE,
} State;

typedef struct serverstate {
    int fd;
    net_buffer_t nb;
    char recvbuf[MAX_LINE_LENGTH + 1];
    char *words[MAX_LINE_LENGTH];
    int nwords;
    State state;
    struct utsname my_uname;
    // TODO: Add additional fields as necessary
    char* username;
    mail_list_t mail;
} serverstate;

static void handle_client(void *new_fd);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }
    run_server(argv[1], handle_client);
    return 0;
}

// syntax_error returns
//   -1 if the server should exit
//    1 otherwise
int syntax_error(serverstate *ss) {
    if (send_formatted(ss->fd, "-ERR %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
    return 1;
}

// checkstate returns
//   -1 if the server should exit
//    0 if the server is in the appropriate state
//    1 if the server is not in the appropriate state
int checkstate(serverstate *ss, State s) {
    if (ss->state != s) {
        if (send_formatted(ss->fd, "-ERR %s\r\n", "Bad sequence of commands") <= 0) return -1;
        return 1;
    }
    return 0;
}

// All the functions that implement a single command return
//   -1 if the server should exit
//    0 if the command was successful
//    1 if the command was unsuccessful

int do_quit(serverstate *ss) {
    // Note: This method has been filled in intentionally!
    dlog("Executing quit\n");
    send_formatted(ss->fd, "+OK Service closing transmission channel\r\n");
    if (ss->state == TRANSACTION) {
        ss->state = UPDATE;
    } else {
        ss->state = Undefined;
    }
    return -1;
}

int do_user(serverstate *ss) {
    dlog("Executing user\n");
    // TODO: Implement this function
    if (ss->nwords != 2) {
        return syntax_error(ss);
    }
    if (ss->state != AUTHORIZATION) {
        return checkstate(ss, AUTHORIZATION) < 0 ? -1 : 1;
    }

    if (is_valid_user(ss->words[1], NULL) == 0) {
        if (send_formatted(ss->fd, "-ERR Invalid user \r\n") <= 0) {
            return -1;
        }
        return 1;
    }

    ss->username = strdup(ss->words[1]);
    if (send_formatted(ss->fd, "+OK User is valid, proceed with password\r\n") <= 0) {
        return -1;
    }
    return 0;
}

int do_pass(serverstate *ss) {
    dlog("Executing pass\n");
    // TODO: Implement this function
    if (ss->nwords != 2) {
        return syntax_error(ss);
    }
    if (ss->state != AUTHORIZATION || ss->username == NULL) {
        if (send_formatted(ss->fd, "-ERR Bad sequence of commands\r\n") <= 0) {
            return -1;
        }
        return 1;
    }
    if (is_valid_user(ss->username, ss->words[1]) == 0) {
        if (send_formatted(ss->fd, "-ERR Invalid passsword\r\n") <= 0) {
            return -1;
        }
        return 1;
    }

    ss->state = TRANSACTION;
    ss->mail = load_user_mail(ss->username);
    if (send_formatted(ss->fd, "+OK Password is valid, mail loaded\r\n") <= 0) {
        return -1;
    }
    return 0;
}

int do_stat(serverstate *ss) {
    dlog("Executing stat\n");
    // TODO: Implement this function
    if (ss->nwords != 1) {
        return syntax_error(ss);
    }
    if (ss->state != TRANSACTION) {
        return checkstate(ss, TRANSACTION);
    }

    int len = mail_list_length(ss->mail, 0);
    size_t s = mail_list_size(ss->mail);
    if (send_formatted(ss->fd, "+OK %d %zu\r\n", len, s) <= 0) {
        return -1;
    }
    return 0;
}

int do_list(serverstate *ss) {
    dlog("Executing list\n");
    // TODO: Implement this function
    if (ss->state != TRANSACTION) {
        return checkstate(ss, TRANSACTION);
    }
    if (ss->nwords != 1 && ss->nwords != 2) {
        return syntax_error(ss);
    }
    int total = mail_list_length(ss->mail, 1);

    if (ss->nwords == 1) {
        int len = mail_list_length(ss->mail, 0);
        size_t s = mail_list_size(ss->mail);

        if (send_formatted(ss->fd, "+OK %d messages (%zu octets)\r\n", len, s) <= 0) {
            return -1;
        }
        for (int i = 0; i < total; i++) {
            mail_item_t item = mail_list_retrieve(ss->mail, (unsigned)i);
            if (item) {
                if (send_formatted(ss->fd, "%d %zu\r\n", i + 1, mail_item_size(item)) <= 0) {
                    return -1;
                }
            }
        }
        if (send_formatted(ss->fd, ".\r\n") <= 0) {
            return -1;
        }
    } else {
        int msg_num = atoi(ss->words[1]);

        if (msg_num > total || msg_num < 1) {
            return syntax_error(ss);
        }
        mail_item_t item = mail_list_retrieve(ss->mail, msg_num - 1);

        if (!item) {
            if (send_formatted(ss->fd, "-ERR No such message\r\n") <= 0) {
                return -1;
            }
            return 1;
        }
        if (send_formatted(ss->fd, "+OK %d %zu\r\n", msg_num, mail_item_size(item)) <= 0) {
            return -1;
        }
    }

    return 0;
}

int do_retr(serverstate *ss) {
    dlog("Executing retr\n");
    // TODO: Implement this function
    if (ss->state != TRANSACTION) {
        return checkstate(ss, TRANSACTION);
    }
    if (ss->nwords != 2) {
        return syntax_error(ss);
    }
    int msg_num = atoi(ss->words[1]);
    int total = mail_list_length(ss->mail, 1);

    if (msg_num < 1 || msg_num > total) {
        return syntax_error(ss);
    }
    mail_item_t item = mail_list_retrieve(ss->mail, (unsigned)(msg_num - 1));

    if (!item) {
        if (send_formatted(ss->fd, "-ERR No such message\r\n") <= 0) {
            return -1;
        }
        return 1;
    }
    FILE *f = mail_item_contents(item);

    if (!f) {
        if (send_formatted(ss->fd, "-ERR Cannot open message\r\n") <= 0) {
            return -1;
        }
        return 1;
    }
    if (send_formatted(ss->fd, "+OK Message follows\r\n") <= 0) {
        fclose(f);
        return -1;
    }
    char buf[MAX_LINE_LENGTH];
    size_t s;

    while ((s = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(ss->fd, buf, s) < 0) {
            fclose(f);
            if (send_formatted(ss->fd, "-ERR Error retrieving message\r\n") <= 0) {
                return -1;
            }
            return 1;
        }
    }

    fclose(f);
    if (send_formatted(ss->fd, ".\r\n") <= 0) {
        return -1;
    }
    return 0;
}

int do_rset(serverstate *ss) {
    dlog("Executing rset\n");
    // TODO: Implement this function
    if (ss->state != TRANSACTION) {
        return checkstate(ss, TRANSACTION);
    }
    if (ss->nwords != 1) {
        return syntax_error(ss);
    }
    int restored = mail_list_undelete(ss->mail);

    if (send_formatted(ss->fd, "+OK %d messages restored\r\n", restored) <= 0) {
        return -1;
    }
    return 0;
}

int do_noop(serverstate *ss) {
    dlog("Executing noop\n");
    // TODO: Implement this function
    if (ss->nwords != 1) {
        return syntax_error(ss);
    }
    if (send_formatted(ss->fd, "+OK (noop)\r\n") <= 0) {
        return -1;
    }
    return 0;
}

int do_dele(serverstate *ss) {
    dlog("Executing dele\n");
    // TODO: Implement this function
    if (ss->state != TRANSACTION) {
        return checkstate(ss, TRANSACTION);
    }
    if (ss->nwords != 2) {
        return syntax_error(ss);
    }
    int msg_num = atoi(ss->words[1]);
    int total = mail_list_length(ss->mail, 1);

    if (msg_num < 1 || msg_num > total) {
        return syntax_error(ss);
    }
    mail_item_t item = mail_list_retrieve(ss->mail, (unsigned)(msg_num - 1));

    if (!item) {
        if (send_formatted(ss->fd, "-ERR Message already deleted\r\n") <= 0) {
            return -1;
        }
        return 1;
    }

    mail_item_delete(item);
    if (send_formatted(ss->fd, "+OK Message deleted\r\n") <= 0) {
        return -1;
    }
    return 0;
}

void handle_client(void *new_fd) {
    int fd = *(int *)(new_fd);

    size_t len;
    serverstate mstate, *ss = &mstate;

    ss->fd = fd;
    ss->nb = nb_create(fd, MAX_LINE_LENGTH);
    ss->state = Undefined;
    uname(&ss->my_uname);
    // TODO: Initialize additional fields in `serverstate`, if any
    if (send_formatted(fd, "+OK POP3 Server on %s ready\r\n", ss->my_uname.nodename) <= 0) return;
    ss->state = AUTHORIZATION;
    ss->username = NULL;
    ss->mail = NULL;

    while ((len = nb_read_line(ss->nb, ss->recvbuf)) >= 0) {
        if (ss->recvbuf[len - 1] != '\n') {
            // command line is too long, stop immediately
            send_formatted(fd, "-ERR Syntax error, command unrecognized\r\n");
            break;
        }
        if (strlen(ss->recvbuf) < len) {
            // received null byte somewhere in the string, stop immediately.
            send_formatted(fd, "-ERR Syntax error, command unrecognized\r\n");
            break;
        }
        // Remove CR, LF and other space characters from end of buffer
        while (isspace(ss->recvbuf[len - 1])) ss->recvbuf[--len] = 0;

        dlog("%x: Command is %s\n", fd, ss->recvbuf);
        if (strlen(ss->recvbuf) == 0) {
            send_formatted(fd, "-ERR Syntax error, blank command unrecognized\r\n");
            break;
        }
        // Split the command into its component "words"
        ss->nwords = split(ss->recvbuf, ss->words);
        char *command = ss->words[0];

        /* TODO: Handle the different values of `command` and dispatch it to the correct implementation
         *  TOP, UIDL, APOP commands do not need to be implemented and therefore may return an error response */
        
        int res;
        
        if (!command) {
            res = syntax_error(ss);
        } else if (strcasecmp(command, "QUIT") == 0) {
            res = do_quit(ss);
        } else if (strcasecmp(command, "USER") == 0) {
            res = do_user(ss);
        } else if (strcasecmp(command, "PASS") == 0) {
            res = do_pass(ss);
        } else if (strcasecmp(command, "STAT") == 0) {
            res = do_stat(ss);
        } else if (strcasecmp(command, "LIST") == 0) {
            res = do_list(ss);
        } else if (strcasecmp(command, "RETR") == 0) {
            res = do_retr(ss);
        } else if (strcasecmp(command, "DELE") == 0) {
            res = do_dele(ss);
        } else if (strcasecmp(command, "RSET") == 0) {
            res = do_rset(ss);
        } else if (strcasecmp(command, "NOOP") == 0) {
            res = do_noop(ss);
        } else {
            if (send_formatted(fd, "-ERR Unknown command\r\n") <= 0) {
                res = -1;
            }
            res = 1;
        }

        if (res < 0) {
            break;
        }
    }
    // TODO: Clean up fields in `serverstate`, if required
    if (ss->mail) {
        if (ss->state != UPDATE) {
            mail_list_undelete(ss->mail);
        }
        mail_list_destroy(ss->mail);
        ss->mail = NULL;
    }
    free(ss->username);
    nb_destroy(ss->nb);
    close(fd);
    free(new_fd);
}
