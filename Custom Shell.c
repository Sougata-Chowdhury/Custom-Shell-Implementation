#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_LINE_LEN 1024
#define MAX_ARGS 128
#define MAX_HISTORY 100

#define OP_SEQ 0
#define OP_AND 1
#define OP_OR 2

static char *history[MAX_HISTORY];
static int history_count = 0;

void main_loop(void);
void handle_sigint(int signo);
void parse_line(char *line);
int execute_commands(char **cmd_list, int cmd_count, int *operators);
int execute_single_command(char *cmd);
int execute_pipeline(char **commands, int n);
void strip_surrounding_quotes(char *str);
void add_to_history(const char *cmd);

int main(void)
{
    umask(0);
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    main_loop();
    for (int i = 0; i < history_count; i++) {
        free(history[i]);
    }
    return 0;
}

void main_loop(void)
{
    while (1) {
        char line[MAX_LINE_LEN];
        printf("sh> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        size_t ln = strlen(line);
        if (ln > 0 && line[ln-1] == '\n') {
            line[ln-1] = '\0';
        }
        if (strcmp(line, "") == 0) {
            continue;
        }
        add_to_history(line);
        parse_line(line);
    }
}

void handle_sigint(int signo)
{
    write(STDOUT_FILENO, "\nsh> ", 5);
    fflush(stdout);
}

void parse_line(char *line)
{
    char *cmd_list[128];
    int operators[128];
    int count = 0;
    memset(cmd_list, 0, sizeof(cmd_list));
    memset(operators, -1, sizeof(operators));
    char *start = line;
    while (*start) {
        char *sc = strchr(start, ';');
        char *amp = strstr(start, "&&");
        char *or_ = strstr(start, "||");
        char *delim = NULL;
        int op_type = -1;
        if (sc && (!delim || sc < delim)) {
            delim = sc;
            op_type = OP_SEQ;
        }
        if (amp && (!delim || amp < delim)) {
            delim = amp;
            op_type = OP_AND;
        }
        if (or_ && (!delim || or_ < delim)) {
            delim = or_;
            op_type = OP_OR;
        }
        if (!delim) {
            cmd_list[count++] = strdup(start);
            break;
        } else {
            int length = delim - start;
            char tmp[1024];
            strncpy(tmp, start, length);
            tmp[length] = '\0';
            cmd_list[count] = strdup(tmp);
            operators[count] = op_type;
            count++;
            if (op_type == OP_AND || op_type == OP_OR) {
                start = delim + 2;
            } else {
                start = delim + 1;
            }
            while (*start == ' ') start++;
        }
    }
    if (count == 0) return;
    execute_commands(cmd_list, count, operators);
    for (int i = 0; i < count; i++) {
        free(cmd_list[i]);
    }
}

int execute_commands(char **cmd_list, int cmd_count, int *operators)
{
    int status = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (i > 0) {
            int prev_op = operators[i - 1];
            if (prev_op == OP_AND && status != 0) {
                continue;
            } else if (prev_op == OP_OR && status == 0) {
                continue;
            }
        }
        status = execute_single_command(cmd_list[i]);
    }
    return status;
}

int execute_single_command(char *cmd)
{
    char *commands[64];
    int n = 0;
    commands[n++] = strtok(cmd, "|");
    while (1) {
        char *p = strtok(NULL, "|");
        if (!p) break;
        commands[n++] = p;
    }
    commands[n] = NULL;
    if (n > 1) return execute_pipeline(commands, n);
    {
        char *single = commands[0];
        if (!single) return 0;
        char temp_buf[1024];
        strncpy(temp_buf, single, sizeof(temp_buf));
        temp_buf[sizeof(temp_buf)-1] = '\0';
        char *args[MAX_ARGS];
        int arg_count = 0;
        int in_redir = 0, out_redir = 0, append_redir = 0;
        char *infile = NULL, *outfile = NULL;
        char *token = strtok(temp_buf, " \t");
        while (token) {
            if (strcmp(token, "<") == 0) {
                in_redir = 1;
                token = strtok(NULL, " \t");
                if (token) infile = token;
            } else if (strcmp(token, ">") == 0) {
                out_redir = 1;
                token = strtok(NULL, " \t");
                if (token) outfile = token;
            } else if (strcmp(token, ">>") == 0) {
                append_redir = 1;
                token = strtok(NULL, " \t");
                if (token) outfile = token;
            } else {
                strip_surrounding_quotes(token);
                args[arg_count++] = token;
            }
            token = strtok(NULL, " \t");
        }
        args[arg_count] = NULL;
        if (arg_count == 0) {
            return 0;
        }
        if (strcmp(args[0], "cd") == 0 ||
            strcmp(args[0], "exit") == 0 ||
            strcmp(args[0], "history") == 0 ||
            strcmp(args[0], "clr") == 0 ||
            strcmp(args[0], "help") == 0) {
            if (strcmp(args[0], "cd") == 0) {
                if (!args[1]) {
                    char *home = getenv("HOME");
                    if (home) {
                        if (chdir(home) != 0) {
                            perror("cd");
                            return 1;
                        }
                    }
                } else {
                    if (chdir(args[1]) != 0) {
                        perror("cd");
                        return 1;
                    }
                }
                return 0;
            } else if (strcmp(args[0], "exit") == 0) {
                exit(0);
            } else if (strcmp(args[0], "history") == 0) {
                for (int i = 0; i < history_count; i++) {
                    printf("%d  %s\n", i+1, history[i]);
                }
                return 0;
            } else if (strcmp(args[0], "clr") == 0) {
                system("clear");
                return 0;
            } else if (strcmp(args[0], "help") == 0) {
                printf("Built-in commands:\n");
                printf("cd [dir]          Change directory (no quotes required)\n");
                printf("exit              Exit the shell\n");
                printf("history           Show command history\n");
                printf("clr               Clear the terminal screen\n");
                printf("help              Display this help message and list common system commands\n\n");
                printf("Common system commands available:\n");
                printf("uname             Show system name\n");
                printf("whoami            Show current user\n");
                printf("man [cmd]         Display the manual for a command\n");
                printf("touch [file]      Create an empty file\n");
                printf("cp [src] [dest]   Copy file(s)\n");
                printf("rm [file]         Remove a file\n");
                printf("mv [src] [dest]   Move or rename file(s)\n");
                printf("chmod [mode] [file] Change file permissions\n");
                printf("head [file]       Show the first lines of a file\n");
                printf("tail [file]       Show the last lines of a file\n");
                printf("sort [file]       Sort contents of a file\n");
                printf("wc [file]         Count lines, words, bytes in a file\n");
                printf("grep [pattern] [file] Search for a pattern in a file\n");
                printf("find [dir] [options]  Search for files in a directory\n");
                printf("bc                Start calculator (try: echo \"3+5\" | bc)\n");
                printf("factor [num]      Factorize a number into primes\n");
                return 0;
            }
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            if (in_redir && infile) {
                int fd_in = open(infile, O_RDONLY);
                if (fd_in < 0) {
                    perror("open for input");
                    exit(1);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
            if (out_redir && outfile) {
                int fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0777);
                if (fd_out < 0) {
                    perror("open for output");
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            } else if (append_redir && outfile) {
                int fd_out = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0777);
                if (fd_out < 0) {
                    perror("open for append");
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }
            execvp(args[0], args);
            perror("execvp");
            exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return 1;
    }
}

int execute_pipeline(char **commands, int n)
{
    int pipefd[2 * (n - 1)];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipefd + 2 * i) < 0) {
            perror("pipe");
            return 1;
        }
    }
    int status = 0;
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        } else if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            if (i > 0)
                dup2(pipefd[2 * (i - 1)], STDIN_FILENO);
            if (i < n - 1)
                dup2(pipefd[2 * i + 1], STDOUT_FILENO);
            for (int j = 0; j < 2 * (n - 1); j++)
                close(pipefd[j]);
            char *segment = commands[i];
            char *args[MAX_ARGS];
            int arg_count = 0;
            int in_redir = 0, out_redir = 0, append_redir = 0;
            char *infile = NULL, *outfile = NULL;
            char *tok = strtok(segment, " \t");
            while (tok) {
                if (strcmp(tok, "<") == 0) {
                    in_redir = 1;
                    tok = strtok(NULL, " \t");
                    if (tok) infile = tok;
                } else if (strcmp(tok, ">") == 0) {
                    out_redir = 1;
                    tok = strtok(NULL, " \t");
                    if (tok) outfile = tok;
                } else if (strcmp(tok, ">>") == 0) {
                    append_redir = 1;
                    tok = strtok(NULL, " \t");
                    if (tok) outfile = tok;
                } else {
                    strip_surrounding_quotes(tok);
                    args[arg_count++] = tok;
                }
                tok = strtok(NULL, " \t");
            }
            args[arg_count] = NULL;
            if (arg_count == 0) {
                exit(0);
            }
            if (in_redir && infile) {
                int fd_in = open(infile, O_RDONLY);
                if (fd_in < 0) {
                    perror("open for input");
                    exit(1);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
            if (out_redir && outfile) {
                int fd_out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0777);
                if (fd_out < 0) {
                    perror("open for output");
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            } else if (append_redir && outfile) {
                int fd_out = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0777);
                if (fd_out < 0) {
                    perror("open for append");
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }
            execvp(args[0], args);
            perror("execvp");
            exit(1);
        }
    }
    for (int i = 0; i < 2 * (n - 1); i++)
        close(pipefd[i]);
    for (int i = 0; i < n; i++) {
        int child_status = 0;
        wait(&child_status);
        if (WIFEXITED(child_status)) {
            status = WEXITSTATUS(child_status);
        } else {
            status = 1;
        }
    }
    return status;
}

void strip_surrounding_quotes(char *str)
{
    size_t len = strlen(str);
    if (len < 2)
        return;
    char first = str[0];
    char last = str[len - 1];
    if ((first == '\"' && last == '\"') || (first == '\'' && last == '\'')) {
        memmove(str, str + 1, len - 1);
        str[len - 1] = '\0';
    }
}

void add_to_history(const char *cmd)
{
    if (history_count < MAX_HISTORY) {
        history[history_count++] = strdup(cmd);
    } else {
        free(history[0]);
        for (int i = 1; i < MAX_HISTORY; i++) {
            history[i - 1] = history[i];
        }
        history[MAX_HISTORY - 1] = strdup(cmd);
    }
}
