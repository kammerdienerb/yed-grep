#include <yed/plugin.h>

#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static pthread_t           run_thr;
static char                cmd_to_run[4096];
static int                 control_pipe[2];
static pthread_mutex_t     mtx = PTHREAD_MUTEX_INITIALIZER;
static int                 cmd_running;
static int                 cmd_status;
static int                 cmd_output_ready;
static array_t             cmd_output;
static unsigned long long  cmd_start_time;
static int                 buff_cleared;
static int                 put_please_wait;
static int                 live;
static char               *prg;
static char               *save_current_search;

void grep(int n_args, char **args);
void grep_start(void);
void grep_cleanup(void);
void grep_take_key(int key);
void grep_run(void);
void grep_select(void);

void grep_key_pressed_handler(yed_event *event);
void grep_pump_handler(yed_event *event);

static void *grep_run_thread(void *arg);

yed_buffer *get_or_make_buff(void) {
    yed_buffer *buff;

    buff = yed_get_buffer("*grep-list");

    if (buff == NULL) {
        buff = yed_create_buffer("*grep-list");
        buff->flags |= BUFF_RD_ONLY | BUFF_SPECIAL;
    }

    return buff;
}

void unload(yed_plugin *self) {
    write(control_pipe[1], "s", 1);
    pthread_join(run_thr, NULL);
    close(control_pipe[0]);
    close(control_pipe[1]);
    array_free(cmd_output);
    if (save_current_search && ys->save_search != save_current_search) {
        free(save_current_search);
    }
}

int yed_plugin_boot(yed_plugin *self) {
    yed_event_handler h;

    YED_PLUG_VERSION_CHECK();

    yed_plugin_set_unload_fn(self, unload);

    h.kind = EVENT_KEY_PRESSED;
    h.fn   = grep_key_pressed_handler;
    yed_plugin_add_event_handler(self, h);

    h.kind = EVENT_PRE_PUMP;
    h.fn   = grep_pump_handler;
    yed_plugin_add_event_handler(self, h);

    cmd_output = array_make(char);

    pipe(control_pipe);
    fcntl(control_pipe[0], F_SETFL, fcntl(control_pipe[0], F_GETFL) | O_NONBLOCK);

    pthread_create(&run_thr, NULL, grep_run_thread, NULL);

    yed_plugin_set_command(self, "grep", grep);

    if (!yed_get_var("grep-prg")) {
        yed_set_var("grep-prg", "grep --exclude-dir={.git} -RHnIs '%' .");
    }

    return 0;
}

void grep(int n_args, char **args) {
    int i;
    int key;

    if (!ys->interactive_command) {
        prg = yed_get_var("grep-prg");
        if (!prg) {
            yed_cerr("'grep-prg' not set");
            return;
        }
        grep_start();
        if (n_args) {
            for (i = 0; i < strlen(args[0]); i += 1) {
                yed_cmd_line_readline_take_key(NULL, (int)args[0][i]);
            }
            array_zero_term(ys->cmd_buff);
            grep_run();
            live = 0;
        } else {
            live = 1;
        }
    } else {
        sscanf(args[0], "%d", &key);
        grep_take_key(key);
    }
}

void grep_cleanup(void) {
    if (ys->save_search != NULL) {
        free(ys->save_search);
    }
    ys->save_search = save_current_search;
}

void grep_start(void) {
    ys->interactive_command = "grep";
    ys->cmd_prompt          = "(grep) ";
    save_current_search = ys->current_search ? strdup(ys->current_search) : NULL;

    get_or_make_buff()->flags &= ~BUFF_RD_ONLY;
    yed_buff_clear_no_undo(get_or_make_buff());
    get_or_make_buff()->flags |= BUFF_RD_ONLY;

    YEXE("special-buffer-prepare-focus", "*grep-list");
    yed_frame_set_buff(ys->active_frame, get_or_make_buff());
    yed_set_cursor_far_within_frame(ys->active_frame, 1, 1);
    yed_clear_cmd_buff();
}

void grep_take_key(int key) {
    yed_buffer *buff;

    switch (key) {
        case ESC:
        case CTRL_C:
            write(control_pipe[1], "c", 1);
            ys->interactive_command = NULL;
            ys->current_search      = NULL;
            yed_clear_cmd_buff();
            YEXE("special-buffer-prepare-unfocus", "*grep-list");
            grep_cleanup();

            buff = get_or_make_buff();
            buff->flags &= ~BUFF_RD_ONLY;
            yed_buff_clear_no_undo(buff);
            buff->flags |= BUFF_RD_ONLY;
            break;
        case ENTER:
            if (!cmd_running) {
                ys->interactive_command = NULL;
                ys->current_search      = NULL;
                yed_clear_cmd_buff();
            }
            break;
        default:
            yed_cmd_line_readline_take_key(NULL, key);
            grep_run();
            break;
    }
}

static void *grep_run_thread(void *arg) {
    int           pid;
    struct pollfd pfds[2];
    char          control;
    int           pipe_fds[2];
    int           status;
    int           total_read;
    int           n_read;
    char          buff[4096];


#define CLEANUP_CURRENT(status_ptr)  \
do {                                 \
    if (pid != 0) {                  \
        kill(-pid, SIGKILL);         \
        waitpid(pid, status_ptr, 0); \
        close(pipe_fds[0]);          \
        pid = 0;                     \
        errno = 0;                   \
    }                                \
                                     \
    cmd_running      = 0;            \
    cmd_output_ready = 0;            \
} while (0)



    (void)arg;

    pid = 0;

    pfds[0].fd      = control_pipe[0];
    pfds[0].events  = POLLIN;
    pfds[0].revents = 0;

    for (;;) {
        control = 0;

        if (poll(pfds, 1 + (pid != 0), -1) <= 0) {
            if (errno == EINTR) {
                continue;
            } else {
                goto out;
            }
        }


        if (pid != 0) {
            if (pfds[1].revents & POLLIN) {

    #define MAX_POLL_READ (16386)

                pthread_mutex_lock(&mtx);
                total_read = 0;
                errno = 0;
                while (total_read < MAX_POLL_READ && (n_read = read(pipe_fds[0], buff, sizeof(buff))) > 0) {
                    array_push_n(cmd_output, buff, n_read);
                    total_read += n_read;
                }
                if (n_read <= 0 && errno != EWOULDBLOCK) {
                    write(control_pipe[1], "w", 1);
                    errno = 0;
                }
                pthread_mutex_unlock(&mtx);

            }

            if (pfds[1].revents & POLLERR || pfds[1].revents & POLLHUP) {
                write(control_pipe[1], "w", 1);
            }
        }


        if (pfds[0].revents & POLLIN) {
            while (read(pfds[0].fd, &control, 1) > 0) {
                switch (control) {
                    case 'r':
                        CLEANUP_CURRENT(NULL);

                        if (pipe(pipe_fds)) {
                            goto out;
                        }
                        fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL) | O_NONBLOCK);

                        pfds[1].fd      = pipe_fds[0];
                        pfds[1].events  = POLLIN | POLLERR | POLLHUP;
                        pfds[1].revents = 0;

                        pthread_mutex_lock(&mtx);

                        pid = fork();
                        if (pid == 0) {
                            setpgid(0, 0);

                            while ((dup2(pipe_fds[1], 1) == -1) && (errno == EINTR)) {}
                            close(pipe_fds[0]);
                            close(pipe_fds[1]);
                            execl("/bin/sh", "sh", "-c", cmd_to_run, NULL);
                            exit(123);
                        } else {
                            setpgid(pid, pid);
                            close(pipe_fds[1]);
                        }

                        array_clear(cmd_output);

                        cmd_running = 1;
                        cmd_status  = 0;

                        pthread_mutex_unlock(&mtx);

                        break;

                    case 'w':
                        if (pid != 0) {
                            CLEANUP_CURRENT(&status);
                            if (WIFEXITED(status)) {
                                cmd_status = WEXITSTATUS(status);
                                cmd_output_ready = 1;
                                yed_force_update();
                            }
                        }
                        break;

                    case 'c':
                        CLEANUP_CURRENT(NULL);
                        break;

                    case 's':
                        CLEANUP_CURRENT(NULL);
                        goto out;
                        break;

                }
            }
        }
    }

out:;
    return NULL;

#undef MAX_POLL_READ
#undef CLEANUP_CURRENT
}

void grep_run(void) {
    char       *pattern;
    yed_buffer *buff;
    int         len;

    array_zero_term(ys->cmd_buff);

    cmd_to_run[0]      = 0;
    pattern            = array_data(ys->cmd_buff);
    ys->current_search = pattern;

    if (strlen(pattern) == 0) {
        write(control_pipe[1], "c", 1);

        buff = get_or_make_buff();

        buff->flags &= ~BUFF_RD_ONLY;
        yed_buff_clear_no_undo(buff);
        buff->flags |= BUFF_RD_ONLY;

        return;
    }

    pthread_mutex_lock(&mtx);

    len = perc_subst(prg, pattern, cmd_to_run, sizeof(cmd_to_run));

    ASSERT(len > 0, "buff too small for perc_subst");

    strcat(cmd_to_run, " 2>/dev/null");

    pthread_mutex_unlock(&mtx);

    write(control_pipe[1], "r", 1);

    cmd_start_time  = measure_time_now_ms();
    buff_cleared    = 0;
    put_please_wait = 0;
}

void grep_select(void) {
    yed_line *line;
    char      _path[256];
    char     *path,
             *c;
    int       row,
              row_idx;

    path = _path;
    line = yed_buff_get_line(get_or_make_buff(), ys->active_frame->cursor_line);
    array_zero_term(line->chars);

    if (array_len(line->chars) == 0) { return; }

    row_idx = 0;
    array_traverse(line->chars, c) {
        row_idx += 1;
        if (*c == ':') {
            break;
        }
        *path++ = *c;
    }
    *path = 0;
    path  = _path;

    if (row_idx >= 2) {
        if (*path == '.' && *(path + 1) == '/') {
            path += 2;
        }
    }

    sscanf(array_data(line->chars) + row_idx, "%d", &row);

    YEXE("special-buffer-prepare-jump-focus", path);
    YEXE("buffer", path);
    yed_set_cursor_within_frame(ys->active_frame, row, 1);
    grep_cleanup();
}

void grep_key_pressed_handler(yed_event *event) {
    yed_frame *eframe;

    eframe = ys->active_frame;

    if (event->key != ENTER                           /* not the key we want */
    ||  ys->interactive_command                       /* still typing        */
    ||  !eframe                                       /* no frame            */
    ||  !eframe->buffer                               /* no buffer           */
    ||  strcmp(eframe->buffer->name, "*grep-list")) { /* not our buffer      */
        return;
    }

    grep_select();

    event->cancel = 1;
}

void grep_pump_handler(yed_event *event) {
    unsigned long long  elapsed;
    yed_buffer         *buff;

    if (cmd_output_ready) {
        pthread_mutex_lock(&mtx);

        array_zero_term(cmd_output);

        buff = get_or_make_buff();

        buff->flags &= ~BUFF_RD_ONLY;

        if (cmd_status == 2) {
            yed_buff_clear_no_undo(buff);
        } else {
            yed_fill_buff_from_string(buff, array_data(cmd_output), array_len(cmd_output));
        }

        buff->flags |= BUFF_RD_ONLY;

        array_clear(cmd_output);
        cmd_output_ready = 0;

        pthread_mutex_unlock(&mtx);

        yed_force_update();

        if (!live) {
            ys->interactive_command = NULL;
            ys->current_search      = NULL;
            yed_clear_cmd_buff();
        }

    } else if (cmd_running) {
        elapsed = measure_time_now_ms() - cmd_start_time;

        if (!put_please_wait && elapsed > 500) {
            buff = get_or_make_buff();

            buff->flags &= ~BUFF_RD_ONLY;
            yed_buff_clear_no_undo(buff);
            yed_buff_insert_string_no_undo(buff, "Please wait...", 1, 1);
            buff->flags |= BUFF_RD_ONLY;

            put_please_wait = 1;

            yed_force_update();
        } else if (!buff_cleared && elapsed > 50) {
            buff = get_or_make_buff();

            buff->flags &= ~BUFF_RD_ONLY;
            yed_buff_clear_no_undo(buff);
            buff->flags |= BUFF_RD_ONLY;

            buff_cleared = 1;

            yed_force_update();
        }
    }
}
