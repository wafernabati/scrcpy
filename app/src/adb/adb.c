#include "adb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adb_parser.h"
#include "util/file.h"
#include "util/log.h"
#include "util/process_intr.h"
#include "util/str.h"

/* Convenience macro to expand:
 *
 *     const char *const argv[] =
 *         SC_ADB_COMMAND("shell", "echo", "hello");
 *
 * to:
 *
 *     const char *const argv[] =
 *         { sc_adb_get_executable(), "shell", "echo", "hello", NULL };
 */
#define SC_ADB_COMMAND(...) { sc_adb_get_executable(), __VA_ARGS__, NULL }

static const char *adb_executable;

const char *
sc_adb_get_executable(void) {
    if (!adb_executable) {
        adb_executable = getenv("ADB");
        if (!adb_executable)
            adb_executable = "adb";
    }
    return adb_executable;
}

// serialize argv to string "[arg1], [arg2], [arg3]"
static size_t
argv_to_string(const char *const *argv, char *buf, size_t bufsize) {
    size_t idx = 0;
    bool first = true;
    while (*argv) {
        const char *arg = *argv;
        size_t len = strlen(arg);
        // count space for "[], ...\0"
        if (idx + len + 8 >= bufsize) {
            // not enough space, truncate
            assert(idx < bufsize - 4);
            memcpy(&buf[idx], "...", 3);
            idx += 3;
            break;
        }
        if (first) {
            first = false;
        } else {
            buf[idx++] = ',';
            buf[idx++] = ' ';
        }
        buf[idx++] = '[';
        memcpy(&buf[idx], arg, len);
        idx += len;
        buf[idx++] = ']';
        argv++;
    }
    assert(idx < bufsize);
    buf[idx] = '\0';
    return idx;
}

static void
show_adb_installation_msg() {
#ifndef __WINDOWS__
    static const struct {
        const char *binary;
        const char *command;
    } pkg_managers[] = {
        {"apt", "apt install adb"},
        {"apt-get", "apt-get install adb"},
        {"brew", "brew cask install android-platform-tools"},
        {"dnf", "dnf install android-tools"},
        {"emerge", "emerge dev-util/android-tools"},
        {"pacman", "pacman -S android-tools"},
    };
    for (size_t i = 0; i < ARRAY_LEN(pkg_managers); ++i) {
        if (sc_file_executable_exists(pkg_managers[i].binary)) {
            LOGI("You may install 'adb' by \"%s\"", pkg_managers[i].command);
            return;
        }
    }
#endif
}

static void
show_adb_err_msg(enum sc_process_result err, const char *const argv[]) {
#define MAX_COMMAND_STRING_LEN 1024
    char *buf = malloc(MAX_COMMAND_STRING_LEN);
    if (!buf) {
        LOG_OOM();
        LOGE("Failed to execute");
        return;
    }

    switch (err) {
        case SC_PROCESS_ERROR_GENERIC:
            argv_to_string(argv, buf, MAX_COMMAND_STRING_LEN);
            LOGE("Failed to execute: %s", buf);
            break;
        case SC_PROCESS_ERROR_MISSING_BINARY:
            argv_to_string(argv, buf, MAX_COMMAND_STRING_LEN);
            LOGE("Command not found: %s", buf);
            LOGE("(make 'adb' accessible from your PATH or define its full"
                 "path in the ADB environment variable)");
            show_adb_installation_msg();
            break;
        case SC_PROCESS_SUCCESS:
            // do nothing
            break;
    }

    free(buf);
}

static bool
process_check_success_internal(sc_pid pid, const char *name, bool close,
                               unsigned flags) {
    bool log_errors = !(flags & SC_ADB_NO_LOGERR);

    if (pid == SC_PROCESS_NONE) {
        if (log_errors) {
            LOGE("Could not execute \"%s\"", name);
        }
        return false;
    }
    sc_exit_code exit_code = sc_process_wait(pid, close);
    if (exit_code) {
        if (log_errors) {
            if (exit_code != SC_EXIT_CODE_NONE) {
                LOGE("\"%s\" returned with value %" SC_PRIexitcode, name,
                     exit_code);
            } else {
                LOGE("\"%s\" exited unexpectedly", name);
            }
        }
        return false;
    }
    return true;
}

static bool
process_check_success_intr(struct sc_intr *intr, sc_pid pid, const char *name,
                           unsigned flags) {
    if (!sc_intr_set_process(intr, pid)) {
        // Already interrupted
        return false;
    }

    // Always pass close=false, interrupting would be racy otherwise
    bool ret = process_check_success_internal(pid, name, false, flags);

    sc_intr_set_process(intr, SC_PROCESS_NONE);

    // Close separately
    sc_process_close(pid);

    return ret;
}

static sc_pid
sc_adb_execute_p(const char *const argv[], unsigned flags, sc_pipe *pout) {
    unsigned process_flags = 0;
    if (flags & SC_ADB_NO_STDOUT) {
        process_flags |= SC_PROCESS_NO_STDOUT;
    }
    if (flags & SC_ADB_NO_STDERR) {
        process_flags |= SC_PROCESS_NO_STDERR;
    }

    sc_pid pid;
    enum sc_process_result r =
        sc_process_execute_p(argv, &pid, process_flags, NULL, pout, NULL);
    if (r != SC_PROCESS_SUCCESS) {
        // If the execution itself failed (not the command exit code), log the
        // error in all cases
        show_adb_err_msg(r, argv);
        pid = SC_PROCESS_NONE;
    }

    return pid;
}

sc_pid
sc_adb_execute(const char *const argv[], unsigned flags) {
    return sc_adb_execute_p(argv, flags, NULL);
}

bool
sc_adb_forward(struct sc_intr *intr, const char *serial, uint16_t local_port,
               const char *device_socket_name, unsigned flags) {
    char local[4 + 5 + 1]; // tcp:PORT
    char remote[108 + 14 + 1]; // localabstract:NAME
    sprintf(local, "tcp:%" PRIu16, local_port);
    snprintf(remote, sizeof(remote), "localabstract:%s", device_socket_name);

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "forward", local, remote);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb forward", flags);
}

bool
sc_adb_forward_remove(struct sc_intr *intr, const char *serial,
                      uint16_t local_port, unsigned flags) {
    char local[4 + 5 + 1]; // tcp:PORT
    sprintf(local, "tcp:%" PRIu16, local_port);

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "forward", "--remove", local);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb forward --remove", flags);
}

bool
sc_adb_reverse(struct sc_intr *intr, const char *serial,
               const char *device_socket_name, uint16_t local_port,
               unsigned flags) {
    char local[4 + 5 + 1]; // tcp:PORT
    char remote[108 + 14 + 1]; // localabstract:NAME
    sprintf(local, "tcp:%" PRIu16, local_port);
    snprintf(remote, sizeof(remote), "localabstract:%s", device_socket_name);
    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "reverse", remote, local);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb reverse", flags);
}

bool
sc_adb_reverse_remove(struct sc_intr *intr, const char *serial,
                      const char *device_socket_name, unsigned flags) {
    char remote[108 + 14 + 1]; // localabstract:NAME
    snprintf(remote, sizeof(remote), "localabstract:%s", device_socket_name);

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "reverse", "--remove", remote);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb reverse --remove", flags);
}

bool
sc_adb_push(struct sc_intr *intr, const char *serial, const char *local,
            const char *remote, unsigned flags) {
#ifdef __WINDOWS__
    // Windows will parse the string, so the paths must be quoted
    // (see sys/win/command.c)
    local = sc_str_quote(local);
    if (!local) {
        return SC_PROCESS_NONE;
    }
    remote = sc_str_quote(remote);
    if (!remote) {
        free((void *) local);
        return SC_PROCESS_NONE;
    }
#endif

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "push", local, remote);

    sc_pid pid = sc_adb_execute(argv, flags);

#ifdef __WINDOWS__
    free((void *) remote);
    free((void *) local);
#endif

    return process_check_success_intr(intr, pid, "adb push", flags);
}

bool
sc_adb_install(struct sc_intr *intr, const char *serial, const char *local,
               unsigned flags) {
#ifdef __WINDOWS__
    // Windows will parse the string, so the local name must be quoted
    // (see sys/win/command.c)
    local = sc_str_quote(local);
    if (!local) {
        return SC_PROCESS_NONE;
    }
#endif

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "install", "-r", local);

    sc_pid pid = sc_adb_execute(argv, flags);

#ifdef __WINDOWS__
    free((void *) local);
#endif

    return process_check_success_intr(intr, pid, "adb install", flags);
}

bool
sc_adb_tcpip(struct sc_intr *intr, const char *serial, uint16_t port,
             unsigned flags) {
    char port_string[5 + 1];
    sprintf(port_string, "%" PRIu16, port);

    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "tcpip", port_string);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb tcpip", flags);
}

bool
sc_adb_connect(struct sc_intr *intr, const char *ip_port, unsigned flags) {
    const char *const argv[] = SC_ADB_COMMAND("connect", ip_port);

    sc_pipe pout;
    sc_pid pid = sc_adb_execute_p(argv, flags, &pout);
    if (pid == SC_PROCESS_NONE) {
        LOGE("Could not execute \"adb connect\"");
        return false;
    }

    // "adb connect" always returns successfully (with exit code 0), even in
    // case of failure. As a workaround, check if its output starts with
    // "connected".
    char buf[128];
    ssize_t r = sc_pipe_read_all_intr(intr, pid, pout, buf, sizeof(buf) - 1);
    sc_pipe_close(pout);

    bool ok = process_check_success_intr(intr, pid, "adb connect", flags);
    if (!ok) {
        return false;
    }

    if (r == -1) {
        return false;
    }

    assert((size_t) r < sizeof(buf));
    buf[r] = '\0';

    ok = !strncmp("connected", buf, sizeof("connected") - 1);
    if (!ok && !(flags & SC_ADB_NO_STDERR)) {
        // "adb connect" also prints errors to stdout. Since we capture it,
        // re-print the error to stderr.
        size_t len = strcspn(buf, "\r\n");
        buf[len] = '\0';
        fprintf(stderr, "%s\n", buf);
    }
    return ok;
}

bool
sc_adb_disconnect(struct sc_intr *intr, const char *ip_port, unsigned flags) {
    assert(ip_port);
    const char *const argv[] = SC_ADB_COMMAND("disconnect", ip_port);

    sc_pid pid = sc_adb_execute(argv, flags);
    return process_check_success_intr(intr, pid, "adb disconnect", flags);
}

char *
sc_adb_getprop(struct sc_intr *intr, const char *serial, const char *prop,
               unsigned flags) {
    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "shell", "getprop", prop);

    sc_pipe pout;
    sc_pid pid = sc_adb_execute_p(argv, flags, &pout);
    if (pid == SC_PROCESS_NONE) {
        LOGE("Could not execute \"adb getprop\"");
        return NULL;
    }

    char buf[128];
    ssize_t r = sc_pipe_read_all_intr(intr, pid, pout, buf, sizeof(buf) - 1);
    sc_pipe_close(pout);

    bool ok = process_check_success_intr(intr, pid, "adb getprop", flags);
    if (!ok) {
        return NULL;
    }

    if (r == -1) {
        return NULL;
    }

    assert((size_t) r < sizeof(buf));
    buf[r] = '\0';
    size_t len = strcspn(buf, " \r\n");
    buf[len] = '\0';

    return strdup(buf);
}

char *
sc_adb_get_serialno(struct sc_intr *intr, unsigned flags) {
    const char *const argv[] = SC_ADB_COMMAND("get-serialno");

    sc_pipe pout;
    sc_pid pid = sc_adb_execute_p(argv, flags, &pout);
    if (pid == SC_PROCESS_NONE) {
        LOGE("Could not execute \"adb get-serialno\"");
        return NULL;
    }

    char buf[128];
    ssize_t r = sc_pipe_read_all_intr(intr, pid, pout, buf, sizeof(buf) - 1);
    sc_pipe_close(pout);

    bool ok = process_check_success_intr(intr, pid, "adb get-serialno", flags);
    if (!ok) {
        return NULL;
    }

    if (r == -1) {
        return NULL;
    }

    assert((size_t) r < sizeof(buf));
    buf[r] = '\0';
    size_t len = strcspn(buf, " \r\n");
    buf[len] = '\0';

    return strdup(buf);
}

char *
sc_adb_get_device_ip(struct sc_intr *intr, const char *serial, unsigned flags) {
    assert(serial);
    const char *const argv[] =
        SC_ADB_COMMAND("-s", serial, "shell", "ip", "route");

    sc_pipe pout;
    sc_pid pid = sc_adb_execute_p(argv, flags, &pout);
    if (pid == SC_PROCESS_NONE) {
        LOGD("Could not execute \"ip route\"");
        return NULL;
    }

    // "adb shell ip route" output should contain only a few lines
    char buf[1024];
    ssize_t r = sc_pipe_read_all_intr(intr, pid, pout, buf, sizeof(buf) - 1);
    sc_pipe_close(pout);

    bool ok = process_check_success_intr(intr, pid, "ip route", flags);
    if (!ok) {
        return NULL;
    }

    if (r == -1) {
        return NULL;
    }

    assert((size_t) r < sizeof(buf));
    if (r == sizeof(buf) - 1)  {
        // The implementation assumes that the output of "ip route" fits in the
        // buffer in a single pass
        LOGW("Result of \"ip route\" does not fit in 1Kb. "
             "Please report an issue.\n");
        return NULL;
    }

    // It is parsed as a NUL-terminated string
    buf[r] = '\0';

    return sc_adb_parse_device_ip_from_output(buf);
}

bool
sc_adb_is_serial_tcpip(const char *serial) {
    return strchr(serial, ':');
}
