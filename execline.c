#!/usr/bin/env -S ccraft -D_GNU_SOURCE
/* execline — chain-loading command language
 *
 * WHAT IT DOES
 * Compose environment changes, fd redirections, directory changes,
 * conditionals, and data capture without subshells or temp files.
 * Each command sets some state and hands control to the next via
 * argv — no `;`, `&&`, `||`, or `|` needed.
 *
 * WHY
 * State flows through argv, not binaries.  No exec between
 * commands — env, cwd, fds accumulate predictably without
 * subshells, temp variables, or fork overhead.
 *
 * HOW IT WORKS
 * dispatch() picks the next command from argv, calls its handler.
 * Handlers consume their args, modify state, then call dispatch()
 * on the remaining argv.  External programs get execvp.
 *
 * Blocks ({ } in execlineb, flat argv with `''` terminators)
 * group args for conditionals and data capture.  space-prefixed
 * per nesting depth so substitution cannot produce terminators.
 *
 * Substitution ($KEY / ${KEY}) by define, importas, elglob,
 * multisubstitute.  Backslash quoting, split values, simultaneous.
 *
 * USAGE
 *   execline CMD [ARGS...]
 *   ln -s execline CMD; ./CMD [ARGS...]
 *
 * EXIT CODES
 *   0   success
 *   100 syntax / usage error
 *   111 temporary failure (out of memory, syscall)
 *   127 command not found / exec failed
 *   other  command-specific
 *
 * COMMANDS
 *   backtick        Capture stdout into env var
 *   background      Fork block, continue immediately
 *   case            Match value (fnmatch or regex)
 *   cd              chdir + update PWD
 *   define          Substitute $KEY with val in remaining argv
 *   dollarat        Print positional params
 *   elgetpositionals Collect env $N..$M into var
 *   elglob          Glob pattern into env var
 *   eltest          Inline POSIX test(1), no fork
 *   empty           Unset named env vars
 *   emptyenv        Clear entire environment
 *   envfile         Load KEY=val from file
 *   exec            Replace process
 *   execlineb       Parse execline script text
 *   exit            Exit with code
 *   export          setenv(key, val)
 *   fdblock         Clear O_NONBLOCK on fd
 *   fdclose         Close fd
 *   fdmove          dup2(old, new)
 *   fdreserve       Reserve fd (close, open /dev/null)
 *   fdswap          Swap two fds
 *   forbacktickx    Iterate over generator output
 *   foreground      Fork block, wait
 *   forstdin        Iterate over stdin lines
 *   forx            Iterate over literal values
 *   getcwd          Store cwd into env var
 *   getpid          Store PID into env var
 *   heredoc         Feed string as stdin via pipe
 *   if              Run chain if block exits 0
 *   ifelse          Two-branch conditional
 *   ifthenelse      Two-branch conditional + unconditional rest
 *   importas        Import env var, substitute $VAR
 *   multidefine     Define multiple key/val pairs at once
 *   multisubstitute Multiple substitutions in one pass
 *   pipeline        Pipe block stdout to next command
 *   piperw          Create pipe at given fds
 *   posix-cd        chdir, no PWD update
 *   posix-umask     Identical to umask (compat)
 *   redirfd         Redirect fd to file
 *   runblock        Run block with positional args
 *   trap            In-process signal handlers
 *   tryexec         Exec with fallback on failure
 *   umask           Set file creation mask
 *   unexport        unsetenv(key)
 *   wait            Wait for children
 *   withstdinas     Open file as stdin
 *
 */

/* fnmatch.h before ctype.h etc. because those set _POSIX_C_SOURCE
 * which hides FNM_CASEFOLD (glibc guards it behind !_POSIX_C_SOURCE). */
#include <fnmatch.h>
#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0   /* non-GNU: no case-insensitive fnmatch */
#endif
#include <regex.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Predicates — name domain checks, hide character gymnastics         */
/* ------------------------------------------------------------------ */

static int     eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int     has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int     is_empty(const char *s) { return s[0] == '\0'; }
static int     is_opt(const char *s) { return s && s[0] == '-'; }
static int     is_ident_char(int c) { return isalnum(c) || c == '_'; }
static int     child_exit_ok(int s) { return WIFEXITED(s); }
static int     child_signal(int s) { return WIFSIGNALED(s); }
static int     child_code(int s) { return WEXITSTATUS(s); }
static int     child_sigcode(int s) { return 256 + WTERMSIG(s); }

/* Extract exit code from wait status — signal → 256+sig, 127 fallback */
static int
waitstatus_exit_code(int s) {
    if (child_exit_ok(s)) return child_code(s);
    if (child_signal(s)) return child_sigcode(s);
    return 127;
}


#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* ------------------------------------------------------------------ */
/* Safe allocators — fail loud on OOM                                  */
/* ------------------------------------------------------------------ */

static void
die(const char *msg) {
    fprintf(stderr, "execline: fatal: %s\n", msg);
    exit(111);
}

static void
die_sys(void) {
    perror("execline");
    exit(111);
}

static void *
xmalloc(size_t sz) {
    void *p = malloc(sz);
    if (!p) die("out of memory");
    return p;
}

static void *
xrealloc(void *p, size_t sz) {
    p = realloc(p, sz);
    if (!p) die("out of memory");
    return p;
}

static char *
xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

/* Non-fatal allocators — return NULL on failure, no abortion.
 * Used in the substitution engine so that OOM in runtime paths
 * lets the script cope rather than dying. */
static void *
try_malloc(size_t sz) {
    return malloc(sz);
}

static void *
try_realloc(void *p, size_t sz) {
    return realloc(p, sz);
}

static char *
try_strdup(const char *s) {
    return strdup(s);
}

/* Free argv allocated by subst_argv or similar */
static void
free_argv(int argc, char **argv) {
    if (!argv) return;
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

/* Fork + check. Dies on failure, returns pid on success. */
extern char **environ;

static void
env_clear(void) {
    /* Portable clearenv(): set environ to empty list.
     * setenv()/unsetenv() reallocate environ as needed. */
    static char *empty_environ[1] = { NULL };
    environ = empty_environ;
}

static pid_t
fork_or_die(void) {
    pid_t pid = fork();
    if (pid < 0) die_sys();
    return pid;
}

/* Set the ? status variable from an exit code */
static void
set_status_var(int code) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", code);
    setenv("?", buf, 1);
}

/* Return the basename of a path (mutable, no allocation) */
static const char *
path_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Parse a decimal number from s; sets *ok. */
static int
parse_int(const char *s, bool *ok) {
    char *end = NULL;
    long n = strtol(s, &end, 10);
    *ok = end && *end == '\0' && end != s;
    return (int)n;
}

static long
parse_long(const char *s, bool *ok) {
    char *end = NULL;
    long n = strtol(s, &end, 10);
    *ok = end && *end == '\0' && end != s;
    return n;
}

/* Parse comma-separated exit codes into a 256-bit bitmap (map[0..31]). */
static int
parse_code_map(const char *s, unsigned char *map) {
    int n = 0;
    while (*s) {
        bool ok;
        int code = parse_int(s, &ok);
        if (ok && code >= 0 && code < 256) {
            map[code >> 3] |= (unsigned char)(1 << (code & 7));
            n++;
        }
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
        else break;
    }
    return n;
}

/* Check if code is set in a 256-bit bitmap */
static int
in_code_map(const unsigned char *map, int code) {
    return code >= 0 && code < 256 && (map[code >> 3] & (1 << (code & 7)));
}

/* Write all len bytes to fd, retrying on EINTR */
static void
write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            die_sys();
        }
        written += n;
    }
}

/* Join glob results into a space-separated string */
static char *
glob_join(const glob_t *g) {
    size_t total = 0;
    for (size_t j = 0; j < g->gl_pathc; j++)
        total += strlen(g->gl_pathv[j]) + 1;
    char *value = xmalloc(total + 1);
    value[0] = '\0';
    for (size_t j = 0; j < g->gl_pathc; j++) {
        if (j > 0) strcat(value, " ");
        strcat(value, g->gl_pathv[j]);
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Block reading                                                      */
/* ------------------------------------------------------------------ */

/* EXECLINE_STRICT levels: 1=warn, 2=die on unquoted block args */
static int
strict_level(void) {
    const char *v = getenv("EXECLINE_STRICT");
    if (!v) return 0;
    if (eq(v, "1")) return 1;
    if (eq(v, "2")) return 2;
    return 0;
}

/* Boolean wrapper: is strict mode active at the given minimum level? */
static int
is_strict(int min_level) {
    return strict_level() >= min_level;
}

/*
 * In execline's argv format, a block is a sequence of arguments terminated
 * by an empty argument (""). Arguments inside a block may begin with a space
 * to prevent substitution from producing empty words that would terminate
 * the block prematurely.
 *
 * read_block() extracts the block starting at argv[start], returns the
 * block's argv in *block_argc / *block_argv (caller must free *block_argv),
 * and sets *end to the index after the block terminator.
 */
static void
read_block(int argc, char *argv[], int start,
           int *block_argc, char ***block_argv, int *end) {
    int cap = 8;
    int n = 0;
    char **bv = xmalloc(sizeof(char *) * cap);
    int i = start;
    while (i < argc) {
        if (is_empty(argv[i])) {
            i++;
            break;
        }
        /* Check for unquoted block args when strict mode is on.
         * A quoted arg begins with a space. Non-space means unquoted. */
        if (argv[i][0] != ' ' && i > start) {
            if (is_strict(2)) {
                fprintf(stderr, "execline: strict: unquoted arg '%s'\n", argv[i]);
                exit(100);
            }
            if (is_strict(1))
                fprintf(stderr, "execline: warning: unquoted arg '%s'\n", argv[i]);
        }
        if (n >= cap) {
            cap *= 2;
            bv = xrealloc(bv, sizeof(char *) * cap);
        }
        char *s = argv[i];
        while (*s == ' ') s++;
        bv[n] = xstrdup(s);
        n++;
        i++;
    }

    /* NULL-terminate for safety — run_block child passes
     * block_argv to cmd_dispatch which uses execvp for externals. */
    if (n >= cap) {
        cap++;
        bv = xrealloc(bv, sizeof(char *) * cap);
    }
    bv[n] = NULL;

    *block_argc = n;
    *block_argv = bv;
    *end = i;
}

/* Free a block argv */
static void
free_block(int argc, char **argv) {
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

/* ------------------------------------------------------------------ */
/* Substitution engine                                                */
/* ------------------------------------------------------------------ */

/*
 * A single substitution: replace $KEY or ${KEY} in argv strings with VALUE.
 * Supports backslash quoting (odd count of backslashes before $KEY prevents
 * substitution), split values (when split is true, VALUE is split on delim
 * and each split produces a separate word), and recursive substitution
 * across multiple keys (applied simultaneously).
 */

struct subst {
    const char *key;   /* The variable name (without $) */
    const char *value; /* The replacement value, may be NULL for "no word" */
    int split;         /* If true, value is split on delim */
    int crunch;        /* -C: merge consecutive delimiters */
    int chomp;         /* -n: strip trailing delimiter */
    char delim;        /* Delimiter for split */
};

/* Check for backtick -0 hint: return '\0' if EXECLINE_DELIM0_<name> set */
static char
backtick_hint_delim(const char *name) {
    char hint[64];
    snprintf(hint, sizeof(hint), "EXECLINE_DELIM0_%s", name);
    if (getenv(hint)) return '\0';
    return ' ';
}

/* Count backslashes before position p in string cur */
static int
count_bs(const char *cur, const char *p) {
    int n = 0;
    while (p > cur && *(p-1) == '\\') { p--; n++; }
    return n;
}

/* Find all occurrences of $KEY or ${KEY} in cur.
 * Handles backslash quoting: odd backslashes before $KEY means literal.
 * Even backslashes are included in the match span so substitution removes them. */
static int
find_all_keys(const char *cur, const char *key, int keylen,
              const char **matches, int *match_lens, int max) {
    const char *p = cur;
    int n = 0;
    while (*p && n < max) {
        if (*p != '$') { p++; continue; }

        int bs = count_bs(cur, p);
        const char *match_start = p - (bs - bs % 2);  /* include even backslashes */
        int extra = p - match_start;  /* how many backslashes to include */

        if (*(p+1) == '{' && strncmp(p+2, key, keylen) == 0 && *(p+2+keylen) == '}') {
            if (bs % 2 == 1) {
                /* Odd: literal — skip past the backslash-swallowed $KEY */
                p += 3 + keylen;
                continue;
            }
            matches[n] = match_start;
            match_lens[n] = extra + 3 + keylen;
            n++;
            p += 3 + keylen;
            continue;
        }

        if (strncmp(p+1, key, keylen) == 0) {
            char next = *(p+1+keylen);
            if (!is_ident_char(next)) {
                if (bs % 2 == 1) {
                    p += 1 + keylen;
                    continue;
                }
                matches[n] = match_start;
                match_lens[n] = extra + 1 + keylen;
                n++;
                p += 1 + keylen;
                continue;
            }
        }
        p++;
    }
    return n;
}

/* Build one result string by interleaving segments with value parts.
 * Returns NULL on allocation failure. */
static char *
build_result(const char **segments, int *seglens, int ns,
             int *preserve_bs, int nm,
             const char *vstart, int vlen) {
    size_t total = 0;
    for (int i = 0; i < ns; i++) total += seglens[i];
    for (int mi = 0; mi < nm; mi++) total += preserve_bs[mi] + vlen;

    char *r = try_malloc(total + 1);
    if (!r) return NULL;

    char *wp = r;
    for (int i = 0; i < ns; i++) {
        memcpy(wp, segments[i], seglens[i]);
        wp += seglens[i];
        if (i < nm) {
            memset(wp, '\\', preserve_bs[i]);
            wp += preserve_bs[i];
            memcpy(wp, vstart, vlen);
            wp += vlen;
        }
    }
    *wp = '\0';
    return r;
}

/* Substitute a single key in a single word, producing expanded words.
 * All occurrences of $KEY in the word are substituted simultaneously.
 * For split values: every match is replaced with the same split element,
 * producing one output per element (not Cartesian product across matches).
 *
 * Returns NULL on allocation failure (*nout = -1).
 * Returns NULL with *nout = 0 for "no word" deletion.
 * Returns non-NULL with *nout > 0 for expanded words. */
static char **
subst_one_key(const char *cur, const char *key, int keylen,
              const char *val, int split, int crunch, int chomp,
              char delim, int *nout) {
    const char *matches[256];
    int match_lens[256];
    int nm = find_all_keys(cur, key, keylen, matches, match_lens, 256);

    int preserve_bs[256];
    for (int mi = 0; mi < nm; mi++) {
        const char *mp = matches[mi];
        int bs = 0;
        while (*mp == '\\') { bs++; mp++; }
        preserve_bs[mi] = bs / 2;
    }

    if (nm == 0) {
        *nout = 1;
        char **r = try_malloc(sizeof(char *));
        if (!r) { *nout = -1; return NULL; }
        r[0] = try_strdup(cur);
        if (!r[0]) { free(r); *nout = -1; return NULL; }
        return r;
    }

    if (val == NULL) {
        *nout = 0;
        return NULL;
    }

    /* Build segment spans: text between/around matches */
    const char *segments[257];
    int seglens[257];
    int ns = nm + 1;

    segments[0] = cur;
    seglens[0] = matches[0] - cur;
    for (int i = 1; i < nm; i++) {
        int prev_end = matches[i-1] - cur + match_lens[i-1];
        segments[i] = cur + prev_end;
        seglens[i] = matches[i] - cur - prev_end;
    }
    {
        int last_end = matches[nm-1] - cur + match_lens[nm-1];
        segments[nm] = cur + last_end;
        seglens[nm] = strlen(cur) - last_end;
    }

    /* --- Split value path --- */
    if (split && *val) {
        /* Parse split values.
         * When delim == '\0', strchr(v, '\0') always finds the terminator
         * (never returns NULL), so we must iterate by offset instead.
         * Operations occur in this order per execline-transform(7):
         * crunch (merge consecutive delimiters) → chomp (strip trailing
         * delimiter) → split. */
        const char *vparts[256];
        int vpartlens[256];
        int nvals = 0;
        {
            if (delim == '\0') {
                /* Null-delimited: iterate by offset */
                const char *v = val;
                size_t remaining = strlen(val);
                while (remaining > 0 && nvals < 256) {
                    vparts[nvals] = v;
                    vpartlens[nvals] = strlen(v);
                    nvals++;
                    size_t seglen = vpartlens[nvals-1];
                    if (seglen >= remaining) break;
                    v += seglen + 1;
                    remaining -= seglen + 1;
                }
            } else {
                /* Pre-split: chomp trailing delimiter from value */
                size_t vlen = strlen(val);
                const char *vstart = val;
                size_t vremaining = vlen;
                if (chomp && vremaining > 0 && vstart[vremaining-1] == delim)
                    vremaining--;

                const char *v = vstart;
                const char *vend = vstart + vremaining;
                while (v < vend && nvals < 256) {
                    vparts[nvals] = v;
                    const char *next = memchr(v, delim, vend - v);
                    vpartlens[nvals] = next ? (size_t)(next - v) : (size_t)(vend - v);
                    if (next) v = next + 1;
                    else v = vend;
                    /* Crunching: skip empty words unless first */
                    if (!(crunch && nvals > 0 && vpartlens[nvals] == 0))
                        nvals++;
                }
            }
        }

        /* No words after split/chomp/crunch → no word */
        if (nvals == 0) { *nout = 0; return NULL; }

        int out_cap = 64, nout_v = 0;
        char **out = try_malloc(sizeof(char *) * out_cap);
        if (!out) { *nout = -1; return NULL; }

        /* For each split value element, replace ALL matches with that element.
         * Non-last words bounded by next delimiter; last word for non-chomp
         * extends to NUL (preserving ORIG trailing delimiter behavior). */
        for (int vi = 0; vi < nvals; vi++) {
            int vlen = vpartlens[vi];
            if (vi == nvals - 1 && !chomp && vpartlens[vi] > 0) {
                /* Last non-empty word: extend to NUL (includes trailing
                 * delimiter, matching ORIG behavior for "a,b," etc.) */
                vlen = strlen(vparts[vi]);
            }
            char *r = build_result(segments, seglens, ns,
                                    preserve_bs, nm,
                    vparts[vi], vlen);
            if (!r) {
                for (int j = 0; j < nout_v; j++) free(out[j]);
                free(out);
                *nout = -1;
                return NULL;
            }

            if (nout_v >= out_cap) {
                out_cap *= 2;
                char **tmp = try_realloc(out, sizeof(char *) * out_cap);
                if (!tmp) {
                    free(r);
                    for (int j = 0; j < nout_v; j++) free(out[j]);
                    free(out);
                    *nout = -1;
                    return NULL;
                }
                out = tmp;
            }
            out[nout_v++] = r;
        }

        *nout = nout_v;
        return out;
    }

    /* --- Single value path --- */
    {
        int vlen = val ? strlen(val) : 0;
        char *r = build_result(segments, seglens, ns,
                                preserve_bs, nm,
                                val, vlen);
        if (!r) { *nout = -1; return NULL; }

        char **out = try_malloc(sizeof(char *));
        if (!out) { free(r); *nout = -1; return NULL; }
        out[0] = r;
        *nout = 1;
        return out;
    }
}

/* Free a word list from subst_word / subst_argv on error */
/* Substitute all keys in a single word.
 * Returns NULL on OOM (*nresults = -1). */
static char **
subst_word(const char *word, struct subst *substs, int nsubst,
           int *nresults) {
    char *words[256];
    int nw = 0;
    char *w0 = try_strdup(word);
    if (!w0) { *nresults = -1; return NULL; }
    words[nw++] = w0;

    for (int ki = 0; ki < nsubst; ki++) {
        const char *key = substs[ki].key;
        const char *val = substs[ki].value;
        int split = substs[ki].split;
        char delim = substs[ki].delim;
        int keylen = strlen(key);

        char *new_words[256];
        int new_nw = 0;

        for (int wi = 0; wi < nw; wi++) {
            int nout;
            char **expanded = subst_one_key(words[wi], key, keylen,
                                            val, split,
                                            substs[ki].crunch,
                                            substs[ki].chomp,
                                            delim, &nout);

            if (!expanded && nout == -1) {
                /* OOM */
                for (int j = 0; j < new_nw; j++) free(new_words[j]);
                for (int j = 0; j < nw; j++) free(words[j]);
                *nresults = -1;
                return NULL;
            }

            if (!expanded && nout == 0) {
                /* no word */
                continue;
            }

            for (int j = 0; j < nout; j++) {
                if (new_nw >= 256) break;
                new_words[new_nw++] = expanded[j];
            }
            free(expanded);
        }

        for (int j = 0; j < nw; j++) free(words[j]);
        nw = new_nw;
        for (int j = 0; j < nw; j++) words[j] = new_words[j];
    }

    /* Copy to heap-allocated array */
    char **result = try_malloc(sizeof(char *) * nw);
    if (!result) {
        for (int j = 0; j < nw; j++) free(words[j]);
        *nresults = -1;
        return NULL;
    }
    for (int j = 0; j < nw; j++) result[j] = words[j];

    *nresults = nw;
    return result;
}

/*
 * Apply substitutions to an argv array.
 * Returns newly allocated argv, sets *new_argc.
 * Returns NULL on OOM (*new_argc = -1).
 */
static char **
subst_argv(int argc, char *argv[], struct subst *substs, int nsubst,
           int *new_argc) {
    int cap = argc * 2 + 8;
    int n = 0;
    char **result = try_malloc(sizeof(char *) * cap);
    if (!result) { *new_argc = -1; return NULL; }

    for (int i = 0; i < argc; i++) {
        int nw;
        char **w = subst_word(argv[i], substs, nsubst, &nw);

        if (!w && nw == -1) {
            /* OOM */
            for (int j = 0; j < n; j++) free(result[j]);
            free(result);
            *new_argc = -1;
            return NULL;
        }

        for (int j = 0; j < nw; j++) {
            if (n >= cap) {
                cap *= 2;
                char **tmp = try_realloc(result, sizeof(char *) * cap);
                if (!tmp) {
                    free(w[j]);
                    for (int k = j + 1; k < nw; k++) free(w[k]);
                    free(w);
                    for (int k = 0; k < n; k++) free(result[k]);
                    free(result);
                    *new_argc = -1;
                    return NULL;
                }
                result = tmp;
            }
            result[n++] = w[j];
        }
        free(w);
    }

    /* NULL-terminate for execvp */
    if (n >= cap) {
        cap++;
        char **tmp = try_realloc(result, sizeof(char *) * cap);
        if (!tmp) {
            for (int j = 0; j < n; j++) free(result[j]);
            free(result);
            *new_argc = -1;
            return NULL;
        }
        result = tmp;
    }
    result[n] = NULL;

    *new_argc = n;
    return result;
}

/* Forward declaration for dispatch (defined later) */
static int cmd_dispatch(int argc, char *argv[]);

/* ------------------------------------------------------------------ */
/* Foreground-style block execution                                    */
/* ------------------------------------------------------------------ */

/*
 * Execute a block argv as a child process, wait for it.
 * Returns the exit status (256 + signal if killed by signal).
 */
static int run_block(int argc, char *argv[]) {
    pid_t pid = fork_or_die();

    if (pid == 0) {
        /* Child */
        /* If the block has no command, exit 0 */
        if (argc == 0) _exit(0);
        if (eq(argv[0], "")) _exit(0);
        /* Use internal dispatch for execline commands */
        _exit(cmd_dispatch(argc, argv));
    }

    /* Parent */
    int status;
    if (waitpid(pid, &status, 0) < 0) die_sys();

    return waitstatus_exit_code(status);
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Fork/pipe/capture -- shared by backtick and forbacktickx             */
/* ------------------------------------------------------------------ */

/*
 * Fork a child, capture its stdout into a buffer.
 * Returns allocated output in *out (NUL-terminated, must free) and
 * child exit status in *exit_code. Returns NULL in *out on empty output.
 * Dies on system call failure.
 */
static void
capture_output(int argc, char *argv[], char **out, size_t *outlen, int *exit_code) {
    /* Use a temp file instead of a pipe + heap realloc loop.
     * Avoids pipe buffer deadlock on large data and the repeated
     * realloc fragmentation/OOM risk — we allocate once after knowing
     * the size from the file. */
    char tmpname[] = "/tmp/execline-capture-XXXXXX";
    int tmpfd = mkstemp(tmpname);
    if (tmpfd < 0) die_sys();
    unlink(tmpname);

    pid_t pid = fork_or_die();

    if (pid == 0) {
        if (dup2(tmpfd, 1) < 0) _exit(111);
        close(tmpfd);
        if (argc > 0) {
            execvp(argv[0], argv);
            fprintf(stderr, "execline: capture: unable to exec %s: %s\n",
                    argv[0], strerror(errno));
        }
        _exit(127);
    }

    /* Wait for child to finish writing */
    int status;
    waitpid(pid, &status, 0);

    /* Read output from temp file */
    off_t filesize = lseek(tmpfd, 0, SEEK_END);
    if (filesize < 0) die_sys();
    lseek(tmpfd, 0, SEEK_SET);

    char *output = NULL;
    if (filesize > 0) {
        /* Cap at 256MB to prevent silent OOM */
        if ((size_t)filesize > 256 * 1024 * 1024) {
            fprintf(stderr, "execline: capture: output too large (%lld bytes)\n",
                    (long long)filesize);
            close(tmpfd);
            *out = NULL;
            *outlen = 0;
            *exit_code = 111;
            return;
        }
        output = xmalloc((size_t)filesize + 1);
        ssize_t n = read(tmpfd, output, (size_t)filesize);
        if (n < 0) { free(output); output = NULL; filesize = 0; }
        else output[n] = '\0';
    }
    close(tmpfd);

    *out = output;
    *outlen = (filesize > 0) ? (size_t)filesize : 0;
    *exit_code = waitstatus_exit_code(status);
}

/* Chain execution                                                     */
/* ------------------------------------------------------------------ */

/* Forward declarations */
static int cmd_exec(int argc, char *argv[]);
static int cmd_dispatch(int argc, char *argv[]);
static int cmd_define(int, char *[]);

/* Dispatch table entry */
struct cmd_entry {
    const char *name;
    int (*handler)(int argc, char *argv[]);
};

/* Forward declare all handlers */
static int cmd_define(int, char *[]);
static int cmd_importas(int, char *[]);
static int cmd_export(int, char *[]);
static int cmd_unexport(int, char *[]);
static int cmd_redirfd(int, char *[]);
static int cmd_foreground(int, char *[]);
static int cmd_background(int, char *[]);
static int cmd_if(int, char *[]);
static int cmd_ifelse(int, char *[]);
static int cmd_ifthenelse(int, char *[]);
static int cmd_pipeline(int, char *[]);
static int cmd_piperw(int, char *[]);
static int cmd_cd(int, char *[]);
static int cmd_umask(int, char *[]);
static int cmd_getpid(int, char *[]);
static int cmd_backtick(int, char *[]);
static int cmd_forbacktickx(int, char *[]);
static int cmd_elglob(int, char *[]);
static int cmd_case(int, char *[]);
static int cmd_wait(int, char *[]);
static int cmd_trap(int, char *[]);
static int cmd_multisubstitute(int, char *[]);
static int cmd_emptyenv(int, char *[]);
static int cmd_dollarat(int, char *[]);
static int cmd_eltest(int, char *[]);
static int cmd_forx(int, char *[]);
static int cmd_forstdin(int, char *[]);
static int cmd_execlineb(int, char *[]);
static int cmd_elgetpositionals(int, char *[]);
static int cmd_heredoc(int, char *[]);
static int cmd_getcwd(int, char *[]);
static int cmd_fdmove(int, char *[]);
static int cmd_multidefine(int, char *[]);
static int cmd_empty(int, char *[]);
static int cmd_envfile(int, char *[]);
static int cmd_fdblock(int, char *[]);
static int cmd_fdclose(int, char *[]);
static int cmd_fdreserve(int, char *[]);
static int cmd_fdswap(int, char *[]);
static int cmd_posix_cd(int, char *[]);
static int cmd_posix_umask(int, char *[]);
static int cmd_runblock(int, char *[]);
static int cmd_tryexec(int, char *[]);
static int cmd_withstdinas(int, char *[]);

static struct cmd_entry cmd_table[] = {
    {"exec",             cmd_exec},
    {"define",           cmd_define},
    {"importas",         cmd_importas},
    {"export",           cmd_export},
    {"unexport",         cmd_unexport},
    {"redirfd",          cmd_redirfd},
    {"foreground",       cmd_foreground},
    {"background",       cmd_background},
    {"if",               cmd_if},
    {"ifelse",           cmd_ifelse},
    {"ifthenelse",       cmd_ifthenelse},
    {"pipeline",         cmd_pipeline},
    {"piperw",           cmd_piperw},
    {"cd",               cmd_cd},
    {"umask",            cmd_umask},
    {"getpid",           cmd_getpid},
    {"backtick",         cmd_backtick},
    {"forbacktickx",     cmd_forbacktickx},
    {"elglob",           cmd_elglob},
    {"case",             cmd_case},
    {"wait",             cmd_wait},
    {"trap",             cmd_trap},
    {"multisubstitute",  cmd_multisubstitute},
    {"emptyenv",         cmd_emptyenv},
    {"dollarat",         cmd_dollarat},
    {"eltest",           cmd_eltest},
    {"forx",             cmd_forx},
    {"forstdin",         cmd_forstdin},
    {"execlineb",        cmd_execlineb},
    {"elgetpositionals", cmd_elgetpositionals},
    {"heredoc",          cmd_heredoc},
    {"getcwd",           cmd_getcwd},
    {"fdmove",           cmd_fdmove},
    {"multidefine",      cmd_multidefine},
    {"empty",            cmd_empty},
    {"envfile",          cmd_envfile},
    {"fdblock",          cmd_fdblock},
    {"fdclose",          cmd_fdclose},
    {"fdreserve",        cmd_fdreserve},
    {"fdswap",           cmd_fdswap},
    {"posix-cd",         cmd_posix_cd},
    {"posix-umask",      cmd_posix_umask},
    {"runblock",         cmd_runblock},
    {"tryexec",          cmd_tryexec},
    {"withstdinas",      cmd_withstdinas},
};

static struct cmd_entry *find_cmd(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(cmd_table); i++) {
        if (eq(cmd_table[i].name, name))
            return &cmd_table[i];
    }
    return NULL;
}

/* run_chain merged into cmd_dispatch — handling "exit" was the only
 * difference. cmd_dispatch catches it before table lookup. */

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

/* --- exec --- */
static int cmd_exec(int argc, char *argv[]) {
    /* exec [ -c ] [ -l ] [ -a argv0 ] prog... */
    bool opt_c = false;
    bool opt_l = false;
    char *opt_a = NULL;

    /* Simple option parsing */
    int i = 1;
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-c")) { opt_c = true; i++; }
        else if (eq(argv[i], "-l")) { opt_l = true; i++; }
        else if (eq(argv[i], "-a") && i+1 < argc) { opt_a = argv[i+1]; i += 2; }
        else break;
    }

    if (i >= argc) return 0;

    char *exec_path = argv[i];

    if (opt_c) env_clear();

    if (opt_l) {
        /* login shell: prepend - to argv[0] */
        const char *base = path_basename(exec_path);
        char login_name[strlen(base) + 2];
        snprintf(login_name, sizeof(login_name), "-%s", base);
        argv[i] = login_name;
    }

    if (opt_a) argv[i] = opt_a;

    execvp(exec_path, argv + i);
    fprintf(stderr, "execline: unable to exec %s: %s\n", argv[i], strerror(errno));
    return 127;
}

/* --- define --- */
static int cmd_define(int argc, char *argv[]) {
    /* define [ -N | -n ] [ -s ] [ -C | -c ] [ -d delim ] key value prog... */
    bool opt_s = false;
    bool opt_crunch = false;
    bool opt_chomp = false;
    char delim = ' ';
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-n")) { opt_chomp = true; i++; }
        else if (eq(argv[i], "-N")) { opt_chomp = false; i++; }
        else if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else if (eq(argv[i], "-c")) { opt_crunch = false; i++; }
        else if (eq(argv[i], "-C")) { opt_crunch = true; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else break;
    }

    if (i + 2 > argc) {
        fprintf(stderr, "define: usage: define key value prog...\n");
        return 100;
    }

    const char *key = argv[i];
    const char *value = argv[i+1];
    int prog_start = i + 2;
    int nprog = argc - prog_start;

    if (nprog <= 0) return 0;

    /* If -s without explicit -d, check for backtick -0 hint */
    if (opt_s && delim == ' ') delim = backtick_hint_delim(key);

    struct subst subst;
    subst.key = key;
    subst.value = value;
    subst.split = opt_s;
    subst.crunch = opt_crunch;
    subst.chomp = opt_chomp;
    subst.delim = delim;

    int new_argc;
    char **new_argv = subst_argv(nprog, argv + prog_start, &subst, 1, &new_argc);
    if (new_argc == -1) return 111;

    int result = cmd_dispatch(new_argc, new_argv);
    free_argv(new_argc, new_argv);
    return result;
}

/* --- importas --- */
static int cmd_importas(int argc, char *argv[]) {
    /* importas [ -i | -D default ] [ -u ] [ -s ] [ -C | -c ] [ -N | -n ]
     *          [ -d delim ] key envvar prog... */
    bool opt_i = false;
    const char *opt_D = NULL;
    bool opt_u = false;
    bool opt_s = false;
    bool opt_crunch = false;
    bool opt_chomp = false;
    char delim = ' ';
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-i")) { opt_i = true; i++; }
        else if (eq(argv[i], "-u")) { opt_u = true; i++; }
        else if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else if (eq(argv[i], "-C")) { opt_crunch = true; i++; }
        else if (eq(argv[i], "-c")) { opt_crunch = false; i++; }
        else if (eq(argv[i], "-n")) { opt_chomp = true; i++; }
        else if (eq(argv[i], "-N")) { opt_chomp = false; i++; }
        else if (eq(argv[i], "-D") && i+1 < argc) { opt_D = argv[i+1]; i += 2; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else break;
    }

    if (i + 2 > argc) {
        fprintf(stderr, "importas: usage: importas [ -i | -D default ] key envvar prog...\n");
        return 100;
    }

    const char *key = argv[i];
    const char *envvar = argv[i+1];
    int prog_start = i + 2;
    int nprog = argc - prog_start;

    const char *envval = getenv(envvar);
    if (!envval) {
        if (opt_i) {
            fprintf(stderr, "importas: fatal: variable %s is not defined\n", envvar);
            return 100;
        }
        if (opt_D)
            envval = opt_D;
        /* else: "no word" — substitute with NULL */
    }

    if (opt_u)
        unsetenv(envvar);

    if (nprog <= 0) return 0;

    if (!envval && !opt_D) {
        /* "no word": substitute NULL for key, deleting matching words */
        struct subst subst;
        subst.key = key;
        subst.value = NULL;
        subst.split = false;
        subst.crunch = false;
        subst.chomp = false;
        subst.delim = ' ';

        int new_argc;
        char **new_argv = subst_argv(nprog, argv + prog_start, &subst, 1, &new_argc);
        if (new_argc == -1) return 111;
        int result = cmd_dispatch(new_argc, new_argv);
        free_argv(new_argc, new_argv);
        return result;
    }

    /* If -s without explicit -d, check for backtick -0 hint
     * on the source env var (where backtick stored it). */
    if (opt_s && delim == ' ') delim = backtick_hint_delim(envvar);

    struct subst subst;
    subst.key = key;
    subst.value = envval;
    subst.split = opt_s;
    subst.crunch = opt_crunch;
    subst.chomp = opt_chomp;
    subst.delim = delim;

    int new_argc;
    char **new_argv = subst_argv(nprog, argv + prog_start, &subst, 1, &new_argc);
    if (new_argc == -1) return 111;

    int result = cmd_dispatch(new_argc, new_argv);
    free_argv(new_argc, new_argv);
    return result;
}

/* --- export --- */
static int cmd_export(int argc, char *argv[]) {
    /* export var value prog... */
    if (argc < 3) {
        fprintf(stderr, "export: usage: export var value prog...\n");
        return 100;
    }

    if (setenv(argv[1], argv[2], 1) < 0) die_sys();

    return cmd_dispatch(argc - 3, argv + 3);
}

/* --- unexport --- */
static int cmd_unexport(int argc, char *argv[]) {
    /* unexport var prog... */
    if (argc < 2) {
        fprintf(stderr, "unexport: usage: unexport var prog...\n");
        return 100;
    }

    unsetenv(argv[1]);
    return cmd_dispatch(argc - 2, argv + 2);
}

/* --- redirfd --- */
static int cmd_redirfd(int argc, char *argv[]) {
    /* redirfd [ -r | -w | -u | -a | -x ] [ -n ] [ -b ] fd file prog... */
    int mode = -1;
    bool opt_n = false;
    bool opt_b = false;
    bool mode_set = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-r")) { mode = O_RDONLY; mode_set = true; i++; }
        else if (eq(argv[i], "-w")) { mode = O_WRONLY | O_CREAT | O_TRUNC; mode_set = true; i++; }
        else if (eq(argv[i], "-u")) { mode = O_RDWR; mode_set = true; i++; }
        else if (eq(argv[i], "-a")) { mode = O_WRONLY | O_CREAT | O_APPEND; mode_set = true; i++; }
        else if (eq(argv[i], "-x")) { mode = O_WRONLY | O_CREAT | O_EXCL; mode_set = true; i++; }
        else if (eq(argv[i], "-n")) { opt_n = true; i++; }
        else if (eq(argv[i], "-b")) { opt_b = true; i++; }
        else break;
    }

    if (!mode_set || i + 2 >= argc) {
        fprintf(stderr, "redirfd: usage: redirfd [-r|-w|-u|-a|-x] [-n] [-b] fd file prog...\n");
        return 100;
    }

    bool ok;
    int fd = parse_int(argv[i], &ok);
    if (!ok || fd < 0) {
        fprintf(stderr, "redirfd: invalid fd: %s\n", argv[i]);
        return 100;
    }

    const char *file = argv[i+1];
    int prog_start = i + 2;

    /* Determine open flags based on mode and -n */
    int flags = mode;
    if (opt_n) flags |= O_NONBLOCK;

    /* Open the file */
    int newfd = open(file, flags, 0666);
    if (newfd < 0) {
        if ((mode & O_WRONLY) && errno == ENXIO) {
            int tmpfd = open(file, O_RDONLY);
            if (tmpfd >= 0) {
                close(tmpfd);
                newfd = open(file, flags, 0666);
            }
        }
        if (newfd < 0) {
            fprintf(stderr, "redirfd: unable to open %s: %s\n", file, strerror(errno));
            return 111;
        }
    }

    /* Handle -b: change blocking mode after open */
    if (opt_b) {
        int fl = fcntl(newfd, F_GETFL);
        if (fl >= 0) {
            if (opt_n)
                fl &= ~O_NONBLOCK;  /* -n -b: make blocking */
            else
                fl |= O_NONBLOCK;   /* -b: make non-blocking */
            fcntl(newfd, F_SETFL, fl);
        }
    }

    /* Duplicate to the target fd */
    if (dup2(newfd, fd) < 0) {
        fprintf(stderr, "redirfd: unable to dup2: %s\n", strerror(errno));
        close(newfd);
        return 111;
    }

    close(newfd);

    return cmd_dispatch(argc - prog_start, argv + prog_start);
}

/* --- foreground --- */
static int cmd_foreground(int argc, char *argv[]) {
    /* foreground { prog1... } prog2... */
    /* In argv format: foreground <block args terminated by ''> <rest...> */
    if (argc < 2) {
        fprintf(stderr, "foreground: usage: foreground { prog1... } prog2...\n");
        return 100;
    }

    int block_argc;
    char **block_argv;
    int end;

    read_block(argc, argv, 1, &block_argc, &block_argv, &end);

    /* Run the block */
    int status = run_block(block_argc, block_argv);
    free_block(block_argc, block_argv);

    /* Set ? environment variable */
    set_status_var(status);

    /* Run the rest */
    int rest_argc = argc - end;
    return cmd_dispatch(rest_argc, argv + end);
}

/* --- background --- */
static int cmd_background(int argc, char *argv[]) {
    /* background [ -d ] { prog1... } prog2... */
    bool opt_d = false;
    int i = 1;

    if (i < argc && eq(argv[i], "-d")) { opt_d = true; i++; }

    int block_argc;
    char **block_argv;
    int end;

    read_block(argc, argv, i, &block_argc, &block_argv, &end);

    pid_t pid = fork_or_die();

    if (pid == 0) {
        /* Child: run the block via internal dispatch.
         * execvp would require symlinks — dispatch works
         * for both builtins and external commands. */
        if (opt_d) {
            /* Detach: double-fork so grandchild is orphaned and
             * reaped by init — no zombie, no wait needed. */
            pid_t pid2 = fork();
            if (pid2 < 0) _exit(111);
            if (pid2 > 0) _exit(0);
        }
        if (block_argc > 0)
            _exit(cmd_dispatch(block_argc, block_argv));
        _exit(0);
    }

    free_block(block_argc, block_argv);

    if (opt_d) {
        waitpid(pid, NULL, 0);
    } else {
        char pid_str[16];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
        setenv("!", pid_str, 1);
    }

    int rest_argc = argc - end;
    return cmd_dispatch(rest_argc, argv + end);
}

/* Check if pipe_end indicates write endpoint should be connected */
static int
pipe_has_write(int pipe_end) {
    return pipe_end == -1 || pipe_end == 1;
}

/* Check if pipe_end indicates read endpoint should be connected */
static int
pipe_has_read(int pipe_end) {
    return pipe_end == -1 || pipe_end == 0;
}

/* --- if --- */
static int cmd_if(int argc, char *argv[]) {
    /* if [ -n ] [ -X ] [ -t | -x exitcode ] { prog1... } prog2...
     *
     * Per skarnet semantics:
     *   -n   invert condition (succeed on non-zero exit)
     *   -X   don't crash when child killed by signal (no-op)
     *   -t   on failure, exit 0 instead of child's exit code
     *   -x code  on failure, exit code instead of child's exit code */
    bool opt_n = false;
    bool opt_X = false;
    int fail_code = -1;  /* -1 = child exit, 0 = -t, N = -x N */
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-n")) { opt_n = true; i++; }
        else if (eq(argv[i], "-X")) { opt_X = true; i++; }
        else if (eq(argv[i], "-t")) { fail_code = 0; i++; }
        else if (eq(argv[i], "-x") && i+1 < argc) {
            bool ok;
            int code = parse_int(argv[i+1], &ok);
            if (!ok) { fprintf(stderr, "if: invalid exit code: %s\n", argv[i+1]); return 100; }
            fail_code = code;
            i += 2;
        }
        else break;
    }

    int block_argc;
    char **block_argv;
    int end;

    read_block(argc, argv, i, &block_argc, &block_argv, &end);

    int status = run_block(block_argc, block_argv);
    free_block(block_argc, block_argv);

    set_status_var(status);
    (void)opt_X;

    bool condition_true = (status == 0) ? !opt_n : opt_n;

    int rest_argc = argc - end;

    if (condition_true) {
        return cmd_dispatch(rest_argc, argv + end);
    }

    if (fail_code >= 0) return fail_code;
    return status;
}

/* --- ifelse --- */
static int cmd_ifelse(int argc, char *argv[]) {
    /* ifelse [ -X ] [ -n ] { progif... } { progthen... } progelse... */
    bool opt_X = false;
    bool opt_n = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-X")) { opt_X = true; i++; }
        else if (eq(argv[i], "-n")) { opt_n = true; i++; }
        else break;
    }

    int cond_argc;
    char **cond_argv;
    int cond_end;
    read_block(argc, argv, i, &cond_argc, &cond_argv, &cond_end);

    int then_argc;
    char **then_argv;
    int then_end;
    read_block(argc, argv, cond_end, &then_argc, &then_argv, &then_end);

    int condition_status = run_block(cond_argc, cond_argv);
    free_block(cond_argc, cond_argv);

    set_status_var(condition_status);

    bool condition_true = (condition_status == 0) ? !opt_n : opt_n;
    (void)opt_X;

    int rest_argc = argc - then_end;
    char **rest_argv = argv + then_end;

    if (condition_true) {
        /* Execute then-block, then skip else */
        if (then_argc > 0) {
            int status = run_block(then_argc, then_argv);
            free_block(then_argc, then_argv);
            return status;
        }
        free_block(then_argc, then_argv);
        return cmd_dispatch(rest_argc, rest_argv);
    } else {
        /* Skip then-block, run else */
        free_block(then_argc, then_argv);
        return cmd_dispatch(rest_argc, rest_argv);
    }
}

/* Exec the then- or else-block based on condition; free all on failure */
static int
exec_selected_block(int condition_true,
             int then_argc, char **then_argv,
             int else_argc, char **else_argv,
             int rest_argc, char **rest_argv) {
    int block_argc = condition_true ? then_argc : else_argc;
    char **block = condition_true ? then_argv : else_argv;

    if (block_argc > 0) {
        int status = run_block(block_argc, block);
        free_block(then_argc, then_argv);
        free_block(else_argc, else_argv);
        return status;
    }
    free_block(then_argc, then_argv);
    free_block(else_argc, else_argv);
    return cmd_dispatch(rest_argc, rest_argv);
}

/* --- ifthenelse --- */
static int cmd_ifthenelse(int argc, char *argv[]) {
    /* ifthenelse [ -X ] [ -s ] { progif... } { progthen... } { progelse... } prog... */
    bool opt_X = false;
    bool opt_s = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-X")) { opt_X = true; i++; }
        else if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else break;
    }

    int cond_argc;
    char **cond_argv;
    int cond_end;
    read_block(argc, argv, i, &cond_argc, &cond_argv, &cond_end);

    int then_argc;
    char **then_argv;
    int then_end;
    read_block(argc, argv, cond_end, &then_argc, &then_argv, &then_end);

    int else_argc;
    char **else_argv;
    int else_end;
    read_block(argc, argv, then_end, &else_argc, &else_argv, &else_end);

    int condition_status = run_block(cond_argc, cond_argv);
    free_block(cond_argc, cond_argv);

    bool condition_true = (condition_status == 0);
    (void)opt_X;

    if (opt_s) set_status_var(condition_status);

    return exec_selected_block(condition_true,
                               then_argc, then_argv,
                               else_argc, else_argv,
                               argc - else_end, argv + else_end);
}

/* --- pipeline --- */
static int cmd_pipeline(int argc, char *argv[]) {
    /* pipeline [ -d ] [ -r | -w ] { prog1... } prog2... */
    bool opt_d = false;
    int pipe_end = -1; /* -1 = both ends, 0 = read end, 1 = write end */
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-d")) { opt_d = true; i++; }
        else if (eq(argv[i], "-r")) { pipe_end = 0; i++; }
        else if (eq(argv[i], "-w")) { pipe_end = 1; i++; }
        else break;
    }

    int left_argc;
    char **left_argv;
    int left_end;
    read_block(argc, argv, i, &left_argc, &left_argv, &left_end);

    int pfd[2];
    if (pipe(pfd) < 0) die_sys();

    pid_t pid = fork_or_die();

    if (pid == 0) {
        /* Child: runs left command with pipe connected to stdout */
        if (pipe_has_write(pipe_end)) {
            if (dup2(pfd[1], 1) < 0) die_sys();
        }
        if (pipe_has_read(pipe_end)) {
            close(pfd[0]);
        }
        close(pfd[1]);

        if (left_argc > 0) {
            execvp(left_argv[0], left_argv);
            fprintf(stderr, "execline: pipeline: unable to exec %s: %s\n",
                    left_argv[0], strerror(errno));
            _exit(127);
        }
        _exit(0);
    }

    /* Parent */
    close(pfd[1]);
    if (pipe_has_read(pipe_end)) {
        if (dup2(pfd[0], 0) < 0) die_sys();
    }
    close(pfd[0]);

    free_block(left_argc, left_argv);

    int rest_argc = argc - left_end;

    /* Store child PID in ! unless -d (detached) */
    if (!opt_d) {
        char pid_str[32];
        snprintf(pid_str, sizeof(pid_str), "%d", pid);
        setenv("!", pid_str, 1);
    }

    int status = cmd_dispatch(rest_argc, argv + left_end);

    /* Reap the child (non-blocking -- should already be done) */
    waitpid(pid, NULL, WNOHANG);

    return status;
}

/* --- piperw --- */
static int cmd_piperw(int argc, char *argv[]) {
    /* piperw fdr fdw prog... */
    if (argc < 3) {
        fprintf(stderr, "piperw: usage: piperw fdr fdw prog...\n");
        return 100;
    }

    bool ok;
    int fdr = parse_int(argv[1], &ok);
    if (!ok) { fprintf(stderr, "piperw: invalid fdr: %s\n", argv[1]); return 100; }
    int fdw = parse_int(argv[2], &ok);
    if (!ok) { fprintf(stderr, "piperw: invalid fdw: %s\n", argv[2]); return 100; }

    int pfd[2];
    if (pipe(pfd) < 0) die_sys();

    if (dup2(pfd[0], fdr) < 0) die_sys();
    if (dup2(pfd[1], fdw) < 0) die_sys();
    close(pfd[0]);
    close(pfd[1]);

    return cmd_dispatch(argc - 3, argv + 3);
}

/* --- cd --- */
static int cmd_cd(int argc, char *argv[]) {
    /* cd [ path ] prog... */
    if (argc < 2) return cmd_dispatch(0, NULL);

    const char *path = argv[1];

    if (chdir(path) < 0) {
        fprintf(stderr, "execline: cd: %s: %s\n", path, strerror(errno));
        return 111;
    }

    /* Update PWD */
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        setenv("PWD", cwd, 1);

    return cmd_dispatch(argc - 2, argv + 2);
}

/* --- umask --- */
static int cmd_umask(int argc, char *argv[]) {
    /* umask value prog... */
    if (argc < 2) {
        fprintf(stderr, "umask: usage: umask value prog...\n");
        return 100;
    }

    mode_t mask = strtol(argv[1], NULL, 8);

    umask(mask);
    return cmd_dispatch(argc - 2, argv + 2);
}

/* --- getpid --- */
static int cmd_getpid(int argc, char *argv[]) {
    /* getpid [ -E | -e ] [ -P | -p ] var prog... */
    bool do_export = true;  /* -E: export (default), -e: don't export */
    bool opt_p = false;  /* -P: print, -p: don't print */
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-E")) { do_export = true; i++; }
        else if (eq(argv[i], "-e")) { do_export = false; i++; }
        else if (eq(argv[i], "-P")) { opt_p = true; i++; }
        else if (eq(argv[i], "-p")) { opt_p = false; i++; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "getpid: usage: getpid var prog...\n");
        return 100;
    }

    const char *var = argv[i];
    pid_t pid = getpid();

    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);

    if (opt_p) {
        printf("%s\n", pid_str);
    }

    if (do_export) setenv(var, pid_str, 1);

    return cmd_dispatch(argc - i - 1, argv + i + 1);
}

/* --- backtick --- */
static int cmd_backtick(int argc, char *argv[]) {
    /* backtick [ -i | -I | -x | -D default ] [ -N ] [ -E | -e ] [ -0 ]
     *         var { prog1... } prog2... */
    int insist = 2;  /* 2 = error on failure, 1 = accept exit, 0 = ignore */
    const char *opt_D = NULL;
    bool opt_N = false;
    bool opt_0 = false;
    bool do_export = true;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-N")) { opt_N = true; i++; }
        else if (eq(argv[i], "-n")) { i++; }
        else if (eq(argv[i], "-E")) { do_export = true; i++; }
        else if (eq(argv[i], "-e")) { do_export = false; i++; }
        else if (eq(argv[i], "-i")) { insist = 2; i++; }
        else if (eq(argv[i], "-I")) { insist = 0; i++; }
        else if (eq(argv[i], "-x")) { insist = 1; opt_D = NULL; i++; }
        else if (eq(argv[i], "-0")) { opt_0 = true; i++; }
        else if (eq(argv[i], "-D") && i+1 < argc) { insist = 1; opt_D = argv[i+1]; i += 2; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "backtick: usage: backtick var { prog... } prog...\n");
        return 100;
    }

    const char *var = argv[i]; i++;

    int block_argc;
    char **block_argv;
    int block_end;
    read_block(argc, argv, i, &block_argc, &block_argv, &block_end);

    char *output = NULL;
    size_t outlen = 0;
    int exit_code;

    capture_output(block_argc, block_argv, &output, &outlen, &exit_code);

    set_status_var(exit_code);

    char *value;
    char empty_str[] = "";
    bool use_output = false;

    if (exit_code == 0) {
        use_output = true;
    } else if (insist == 0) {
        use_output = true;
    } else if (insist == 1) {
        use_output = opt_D ? false : true;
    } else {
        fprintf(stderr, "backtick: command failed with exit code %d\n", exit_code);
        free(output);
        free_block(block_argc, block_argv);
        return 100;
    }

    if (output && use_output)
        value = output;
    else if (opt_D)
        value = xstrdup(opt_D);
    else
        value = empty_str;

    if (!opt_N && value != empty_str) {
        size_t len = strlen(value);
        char delim = opt_0 ? '\0' : '\n';
        while (len > 0 && value[len-1] == delim)
            len--;
        value[len] = '\0';
    }

    if (do_export) {
        setenv(var, value, 1);
        /* Signal null-delimited origin so define/importas -s
         * can default to \0 delimiter without explicit -d. */
        if (opt_0) {
            char hint[64];
            snprintf(hint, sizeof(hint), "EXECLINE_DELIM0_%s", var);
            setenv(hint, "1", 1);
        }
    }
    if (output && output != value) free(output);
    if (value != empty_str && value != output) free(value);

    free_block(block_argc, block_argv);

    int rest_argc = argc - block_end;
    return cmd_dispatch(rest_argc, argv + block_end);
}

/* --- forbacktickx --- */
static int cmd_forbacktickx(int argc, char *argv[]) {
    /* forbacktickx [ -E | -e ] [ -p | -o codes | -x codes ] [ -N | -n ]
     *               [ -C | -c ] [ -0 | -d delim ] var { gen... } loop... */
    bool opt_c = false;
    char delim = '\n';
    unsigned char opt_o_map[32] = {0};  /* -o: only these codes continue */
    unsigned char opt_x_map[32] = {0};  /* -x: these codes break */
    bool has_opt_o = false;
    bool has_opt_x = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-n") || eq(argv[i], "-N")) { i++; }
        else if (eq(argv[i], "-c")) { opt_c = true; i++; }
        else if (eq(argv[i], "-C")) { i++; }
        else if (eq(argv[i], "-0")) { delim = '\0'; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else if (eq(argv[i], "-E") || eq(argv[i], "-e")) { i++; }
        else if (eq(argv[i], "-p")) { i++; }
        else if (eq(argv[i], "-o") && i+1 < argc) { parse_code_map(argv[i+1], opt_o_map); has_opt_o = true; i += 2; }
        else if (eq(argv[i], "-x") && i+1 < argc) { parse_code_map(argv[i+1], opt_x_map); has_opt_x = true; i += 2; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "forbacktickx: usage: forbacktickx var { gen... } loop...\n");
        return 100;
    }

    const char *var = argv[i]; i++;

    int gen_argc;
    char **gen_argv;
    int gen_end;
    read_block(argc, argv, i, &gen_argc, &gen_argv, &gen_end);

    int loop_start = gen_end;

    char *output = NULL;
    size_t outlen = 0;
    int gen_status;

    capture_output(gen_argc, gen_argv, &output, &outlen, &gen_status);

    free_block(gen_argc, gen_argv);

    /* Handle empty output */
    if (!output || outlen == 0) {
        free(output);
        return cmd_dispatch(argc - gen_end, argv + gen_end);
    }

    /* Strip trailing delimiter */
    while (outlen > 0 && output[outlen-1] == (delim ? delim : '\0'))
        outlen--;

    int last_status = 0;

    if (delim == '\0') {
        /* Null-delimited: iterate by offset */
        size_t pos = 0;
        while (pos < outlen) {
            size_t end = pos;
            while (end < outlen && output[end] != '\0') end++;
            size_t toklen = end - pos;

            if (toklen > 0) {
                output[end] = '\0';
                setenv(var, output + pos, 1);

                if (loop_start < argc) {
                    int loop_status = run_block(argc - loop_start, argv + loop_start);
                    last_status = loop_status;
                    if (has_opt_x && in_code_map(opt_x_map, loop_status)) break;
                    if (has_opt_o && !in_code_map(opt_o_map, loop_status)) break;
                    if (!has_opt_o && !has_opt_x && loop_status != 0 && !opt_c) break;
                }
            }

            pos = end + 1;
        }
    } else {
        /* Character-delimited: use strtok_r */
        output[outlen] = '\0';
        char *save = NULL;
        const char *sep = (delim == '\n') ? "\n" : (char[2]){delim, '\0'};
        char *token = strtok_r(output, sep, &save);

        while (token) {
            setenv(var, token, 1);

            if (loop_start < argc) {
                int loop_status = run_block(argc - loop_start, argv + loop_start);
                last_status = loop_status;
                if (has_opt_x && in_code_map(opt_x_map, loop_status)) break;
                if (has_opt_o && !in_code_map(opt_o_map, loop_status)) break;
                if (!has_opt_o && !has_opt_x && loop_status != 0 && !opt_c) break;
            }

            token = strtok_r(NULL, sep, &save);
        }
    }

    free(output);
    return last_status;
}

/* --- elglob --- */
static int cmd_elglob(int argc, char *argv[]) {
    /* elglob [ -v ] [ -w ] [ -s ] [ -m ] [ -e ] [ -0 ] var pattern prog... */
    bool opt_v = false;  /* -v: print results */
    bool opt_w = false;  /* -w: wildcard only, no substitution */
    bool opt_m = false;  /* -m: match only (no substitution) */
    bool opt_e = false;  /* -e: errors are fatal */
    bool opt_0 = false;  /* -0: null-separated */
    int i = 1;
    int ret = 0;

    bool opt_s = false;  /* -s: mark for split substitution */
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-v")) { opt_v = true; i++; }
        else if (eq(argv[i], "-w")) { opt_w = true; i++; }
        else if (eq(argv[i], "-m")) { opt_m = true; i++; }
        else if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else if (eq(argv[i], "-e")) { opt_e = true; i++; }
        else if (eq(argv[i], "-0")) { opt_0 = true; i++; }
        else break;
    }

    if (i + 2 > argc) {
        fprintf(stderr, "elglob: usage: elglob var pattern prog...\n");
        return 100;
    }

    const char *var = argv[i];
    const char *pattern = argv[i+1];
    int prog_start = i + 2;

    glob_t g = {0};
    int flags = 0;
    if (opt_m) flags |= GLOB_NOCHECK;
    if (opt_e) flags |= GLOB_ERR;
    ret = glob(pattern, flags, NULL, &g);

    if (ret != 0 && ret != GLOB_NOMATCH) {
        if (opt_e) {
            fprintf(stderr, "elglob: glob error for %s\n", pattern);
            globfree(&g);
            return 111;
        }
        /* Non-fatal error: treat as no match. g is zero-initialized
         * so globfree in the no-match path handles cleanup safely. */
        ret = GLOB_NOMATCH;
    }

    if (ret != 0 || g.gl_pathc == 0) {
        /* No match */
        if (opt_w) {
            setenv(var, pattern, 1);
        } else {
            unsetenv(var);
        }
        globfree(&g);
        return cmd_dispatch(argc - prog_start, argv + prog_start);
    }

    char *value = glob_join(&g);

    setenv(var, value, 1);

    /* Mark for split if -s was given (same convention as backtick -0) */
    if (opt_s) {
        if (opt_0) {
            char hint[64];
            snprintf(hint, sizeof(hint), "EXECLINE_DELIM0_%s", var);
            setenv(hint, "1", 1);
        }
    }

    if (opt_v)
        printf("%s\n", value);

    free(value);
    globfree(&g);

    return cmd_dispatch(argc - prog_start, argv + prog_start);
}

/* --- case --- */
static int cmd_case(int argc, char *argv[]) {
    /* case [ -S | -s ] [ -E | -e ] [ -i ] [ -n | -N ] value { pattern { prog... } ... } */
    int mode = 0;       /* 0 = shell, 1 = regex */
    int regex_flags = REG_EXTENDED;
    bool opt_i = false;
    bool opt_n = false;
    bool opt_capture = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-i")) { opt_i = true; i++; }
        else if (eq(argv[i], "-n")) { opt_n = true; i++; }
        else if (eq(argv[i], "-N")) { opt_n = false; opt_capture = true; mode = 1; i++; }
        else if (eq(argv[i], "-s")) { mode = 0; i++; }
        else if (eq(argv[i], "-S")) { mode = 1; i++; }
        else if (eq(argv[i], "-E")) { regex_flags = REG_EXTENDED; mode = 1; i++; }
        else if (eq(argv[i], "-e")) { regex_flags = 0; mode = 1; i++; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "case: usage: case value { pattern { prog... } ... }\n");
        return 100;
    }

    const char *value = argv[i]; i++;
    if (opt_i) regex_flags |= REG_ICASE;

    /* Read the outer block containing pattern/block pairs */
    int block_argc;
    char **block_argv;
    int block_end;
    read_block(argc, argv, i, &block_argc, &block_argv, &block_end);

    int bi = 0;
    int matched = 0;
    while (bi < block_argc && !matched) {
        const char *pattern = block_argv[bi]; bi++;

        /* Read the block that follows this pattern */
        int cmd_argc;
        char **cmd_argv;
        int cmd_end;
        read_block(block_argc, block_argv, bi, &cmd_argc, &cmd_argv, &cmd_end);
        bi = cmd_end;  /* advance past the block */

        int this_match = 0;

        if (mode == 0) {
            int fnflags = 0;
            if (opt_i) fnflags |= FNM_CASEFOLD;
            this_match = (fnmatch(pattern, value, fnflags) == 0);
        } else {
            regex_t re;
            char expr[strlen(pattern) + 3];
            expr[0] = '^';
            memcpy(expr + 1, pattern, strlen(pattern));
            expr[1 + strlen(pattern)] = '$';
            expr[2 + strlen(pattern)] = 0;
            int r = regcomp(&re, expr, regex_flags);
            if (r) {
                char buf[256];
                regerror(r, &re, buf, sizeof(buf));
                fprintf(stderr, "case: invalid regex '%s': %s\n", pattern, buf);
                free_block(cmd_argc, cmd_argv);
                free_block(block_argc, block_argv);
                return 100;
            }
            if (opt_capture) {
                size_t nmatch = re.re_nsub + 1;
                regmatch_t pmatch[nmatch];
                r = regexec(&re, value, nmatch, pmatch, 0);
                if (!r) {
                    this_match = 1;
                    char nstr[16];
                    snprintf(nstr, sizeof(nstr), "%zu", nmatch - 1);
                    setenv("#", nstr, 1);
                    size_t len = pmatch[0].rm_eo - pmatch[0].rm_so;
                    char full[len + 1];
                    memcpy(full, value + pmatch[0].rm_so, len);
                    full[len] = 0;
                    setenv("0", full, 1);
                    for (size_t j = 1; j < nmatch; j++) {
                        char vn[24];
                        snprintf(vn, sizeof(vn), "%zu", j);
                        if (pmatch[j].rm_so >= 0) {
                            size_t clen = pmatch[j].rm_eo - pmatch[j].rm_so;
                            char cap[clen + 1];
                            memcpy(cap, value + pmatch[j].rm_so, clen);
                            cap[clen] = 0;
                            setenv(vn, cap, 1);
                        } else {
                            setenv(vn, "", 1);
                        }
                    }
                }
            } else {
                r = regexec(&re, value, 0, NULL, 0);
                this_match = (r == 0);
            }
            regfree(&re);
        }

        if (opt_n) this_match = !this_match;

        if (this_match) {
            matched = 1;
            if (cmd_argc > 0) {
                int status = run_block(cmd_argc, cmd_argv);
                free_block(cmd_argc, cmd_argv);
                free_block(block_argc, block_argv);
                return status;
            }
            free_block(cmd_argc, cmd_argv);
            break;
        }
        free_block(cmd_argc, cmd_argv);
    }

    free_block(block_argc, block_argv);

    return cmd_dispatch(argc - block_end, argv + block_end);
}

/* Wait for a child pid and optionally set ? status. Returns waitpid result. */
/* Timeout flag for wait -t */
static volatile sig_atomic_t wait_timed_out = 0;

static void
wait_timeout_handler(int signo) {
    (void)signo;
    wait_timed_out = 1;
}

static pid_t
wait_and_set(pid_t pid, int opt_r) {
    int status;
    pid_t result;
    if (wait_timed_out) return -1;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR && !wait_timed_out);
    if (result > 0 && !opt_r)
        set_status_var(waitstatus_exit_code(status));
    return result;
}

/* --- wait --- */
static int cmd_wait(int argc, char *argv[]) {
    /* wait [ -I | -i ] [ -a | -o ] [ -r | -t timeout ] { pids... } prog... */
    bool opt_I = false;
    bool opt_i = false;
    bool opt_r = false;
    bool opt_a = false;
    int opt_timeout = 0;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-I")) { opt_I = true; i++; }
        else if (eq(argv[i], "-i")) { opt_i = true; i++; }
        else if (eq(argv[i], "-a")) { opt_a = true; i++; }
        else if (eq(argv[i], "-o")) { i++; }
        else if (eq(argv[i], "-r")) { opt_r = true; i++; }
        else if (eq(argv[i], "-t") && i+1 < argc) {
            bool ok;
            opt_timeout = parse_int(argv[i+1], &ok);
            if (!ok) { fprintf(stderr, "wait: invalid timeout\n"); return 100; }
            i += 2;
        }
        else break;
    }

    /* Set up timeout if -t was given */
    struct sigaction old_alrm;
    int timer_set = 0;
    if (opt_timeout > 0) {
        wait_timed_out = 0;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = wait_timeout_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGALRM, &sa, &old_alrm);
        alarm(opt_timeout);
        timer_set = 1;
    }

    /* Read optional PIDs block */
    int block_argc = 0;
    char **block_argv = NULL;
    int block_end = i;

    if (i < argc && !is_opt(argv[i])) {
        read_block(argc, argv, i, &block_argc, &block_argv, &block_end);
    }

    if (block_argc > 0) {
        for (int j = 0; j < block_argc; j++) {
            bool ok;
            pid_t pid = parse_int(block_argv[j], &ok);
            if (ok) {
                if (wait_and_set(pid, opt_r) < 0 && wait_timed_out)
                    break;
            }
        }
        free_block(block_argc, block_argv);
    } else {
        pid_t pid;

        if (opt_a) {
            while ((pid = wait_and_set(-1, opt_r)) > 0);
        } else {
            pid = wait_and_set(-1, opt_r);
        }

        if (pid < 0) {
            if (wait_timed_out) {
                wait_timed_out = 0;
            } else if (opt_i) {
                fprintf(stderr, "wait: no children\n");
                return 100;
            } else if (!opt_I) {
                fprintf(stderr, "wait: %s\n", strerror(errno));
                return 111;
            }
        }
    }

    if (timer_set) {
        alarm(0);
        sigaction(SIGALRM, &old_alrm, NULL);
    }

    int rest_argc = argc - block_end;
    return cmd_dispatch(rest_argc, argv + block_end);
}

/* Signal name to number */
static int
sig_num(const char *name) {
    if (has_prefix(name, "SIG")) name += 3;
    if (eq(name, "HUP")) return SIGHUP;
    if (eq(name, "INT")) return SIGINT;
    if (eq(name, "QUIT")) return SIGQUIT;
    if (eq(name, "ILL")) return SIGILL;
    if (eq(name, "ABRT")) return SIGABRT;
    if (eq(name, "FPE")) return SIGFPE;
    if (eq(name, "KILL")) return SIGKILL;
    if (eq(name, "SEGV")) return SIGSEGV;
    if (eq(name, "PIPE")) return SIGPIPE;
    if (eq(name, "ALRM")) return SIGALRM;
    if (eq(name, "TERM")) return SIGTERM;
    if (eq(name, "USR1")) return SIGUSR1;
    if (eq(name, "USR2")) return SIGUSR2;
    if (eq(name, "CHLD")) return SIGCHLD;
    if (eq(name, "CONT")) return SIGCONT;
    if (eq(name, "STOP")) return SIGSTOP;
    if (eq(name, "TSTP")) return SIGTSTP;
    if (eq(name, "WINCH")) return SIGWINCH;
    return -1;
}

/* --- trap (in-process sigaction version) --- */
/*
 * trap [ -x ] { signal { action... } ... } prog...
 *
 * Installs signal handlers in-process using sigaction.
 * When a signal arrives, the handler sets a flag. After the chain completes,
 * pending signals are re-dispatched synchronously.
 *
 * This avoids fork+listen child pattern which had signal race windows.
 * The handlers only set a flag — no async-signal-unsafe work in handler.
 */
struct trap_entry {
    int signo;
    volatile sig_atomic_t fired;
    int action_argc;
    char **action_argv;  /* shallow references into block_argv, not owned */
    struct sigaction old_sa;  /* saved prior handler, restored on teardown */
};

/* Nested inside block to keep multiple trap invocations independent.
 * Must be top-level for signal handler access. */
static struct trap_entry *current_trap_entries = NULL;
static int current_trap_n = 0;

static void
trap_handler(int signo) {
    for (int i = 0; i < current_trap_n; i++) {
        if (current_trap_entries[i].signo == signo) {
            current_trap_entries[i].fired = 1;
            break;
        }
    }
}

/* Dispatch a single action for a trapped signal. Forks to isolate
 * handler execution from the main process. */
static void
trap_dispatch_action(struct trap_entry *entry) {
    if (entry->action_argc == 0) return;

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* Child runs the action via internal dispatch.
         * execvp would require symlinks for execline builtins like
         * "exit". dispatch handles both builtins and external. */
        _exit(cmd_dispatch(entry->action_argc, entry->action_argv));
    }
    /* Block until action completes — WNOHANG would leak zombies
     * if the action hasn't exited by the time we poll. */
    int status;
    waitpid(pid, &status, 0);
}

/* After the chain completes, process any pending trapped signals.
 * Returns the number of signals dispatched (for logging/debug). */
static void
trap_process_pending(void) {
    for (int i = 0; i < current_trap_n; i++) {
        if (current_trap_entries[i].fired) {
            current_trap_entries[i].fired = 0;
            trap_dispatch_action(&current_trap_entries[i]);
        }
    }
}

static int
cmd_trap(int argc, char *argv[]) {
    int i = 1;
    if (i < argc && eq(argv[i], "-x")) { i++; }

    int block_argc; char **block_argv; int block_end;
    read_block(argc, argv, i, &block_argc, &block_argv, &block_end);
    if (block_argc == 0) {
        fprintf(stderr, "trap: fatal: empty block\n");
        return 100;
    }

    /* Parse signal/action pairs into trap_entries */
    int cap = 8;
    struct trap_entry *entries = xmalloc(sizeof(struct trap_entry) * cap);
    int n = 0;

    int bi = 0;
    while (bi < block_argc) {
        const char *name = block_argv[bi]; bi++;
        if (bi >= block_argc) break;

        int ac; char **av; int se;
        read_block(block_argc, block_argv, bi, &ac, &av, &se);

        int signo = sig_num(name);
        if (signo > 0) {
            if (n >= cap) {
                cap *= 2;
                entries = xrealloc(entries, sizeof(struct trap_entry) * cap);
            }
            entries[n].signo = signo;
            entries[n].fired = 0;
            entries[n].action_argc = ac;
            /* Deep copy action argv — block_argv is freed after setup */
            entries[n].action_argv = xmalloc(sizeof(char *) * (ac + 1));
            for (int ai = 0; ai < ac; ai++)
                entries[n].action_argv[ai] = xstrdup(av[ai]);
            entries[n].action_argv[ac] = NULL;
            n++;
        }
        free_block(ac, av);
        bi = se;
    }

    if (n == 0) {
        free(entries);
        free_block(block_argc, block_argv);
        fprintf(stderr, "trap: no valid signals\n");
        return 100;
    }

    /* Install signal handlers using sigaction */
    current_trap_entries = entries;
    current_trap_n = n;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = trap_handler;
    sigfillset(&sa.sa_mask);  /* block all signals during handler */
    sa.sa_flags = SA_RESTART;  /* restart interrupted syscalls */

    for (int si = 0; si < n; si++) {
        /* Initialize so old_sa is valid even for uncatchable signals
         * (SIGKILL/SIGSTOP) where sigaction silently fails. */
        entries[si].old_sa.sa_handler = SIG_DFL;
        sigaction(entries[si].signo, &sa, &entries[si].old_sa);
    }

    /* Run the rest of the chain */
    int rest_argc = argc - block_end;
    int result = cmd_dispatch(rest_argc, argv + block_end);

    /* Process any signals that arrived during execution */
    trap_process_pending();

    /* Restore previous handlers */
    for (int si = 0; si < n; si++) {
        sigaction(entries[si].signo, &entries[si].old_sa, NULL);
        for (int ai = 0; ai < entries[si].action_argc; ai++)
            free(entries[si].action_argv[ai]);
        free(entries[si].action_argv);
    }
    free(entries);
    current_trap_entries = NULL;
    current_trap_n = 0;

    return result;
}

/* Parse a "define" subcommand inside multisubstitute block */
static int
parse_ms_define(char *block_argv[], int block_argc,
                int *bi, struct subst **substs,
                int *nsubst, int *subst_cap) {
    bool split = false;
    char delim = ' ';
    while (*bi < block_argc && is_opt(block_argv[*bi])) {
        if (eq(block_argv[*bi], "-s")) { split = true; (*bi)++; }
        else if (eq(block_argv[*bi], "-d") && *bi+1 < block_argc) { delim = block_argv[*bi+1][0]; *bi += 2; }
        else if (eq(block_argv[*bi], "-n") || eq(block_argv[*bi], "-N")) { (*bi)++; }
        else if (eq(block_argv[*bi], "-c") || eq(block_argv[*bi], "-C")) { (*bi)++; }
        else break;
    }
    if (*bi + 2 > block_argc) return -1;
    const char *key = block_argv[*bi]; (*bi)++;
    const char *value = block_argv[*bi]; (*bi)++;

    if (*nsubst >= *subst_cap) {
        *subst_cap *= 2;
        *substs = xrealloc(*substs, sizeof(struct subst) * *subst_cap);
    }
    if (split && delim == ' ') delim = backtick_hint_delim(key);
    (*substs)[*nsubst].key = xstrdup(key);
    (*substs)[*nsubst].value = xstrdup(value);
    (*substs)[*nsubst].split = split;
    (*substs)[*nsubst].crunch = 0;
    (*substs)[*nsubst].chomp = 0;
    (*substs)[*nsubst].delim = delim;
    (*nsubst)++;
    return 0;
}

/* Parse an "importas" subcommand inside multisubstitute block */
static int
parse_ms_importas(char *block_argv[], int block_argc,
                  int *bi, struct subst **substs,
                  int *nsubst, int *subst_cap) {
    const char *opt_D = NULL;
    bool split = false;
    char delim = ' ';
    bool insist = false;
    while (*bi < block_argc && is_opt(block_argv[*bi])) {
        if (eq(block_argv[*bi], "-s")) { split = true; (*bi)++; }
        else if (eq(block_argv[*bi], "-d") && *bi+1 < block_argc) { delim = block_argv[*bi+1][0]; *bi += 2; }
        else if (eq(block_argv[*bi], "-D") && *bi+1 < block_argc) { opt_D = block_argv[*bi+1]; *bi += 2; }
        else if (eq(block_argv[*bi], "-i")) { insist = true; (*bi)++; }
        else break;
    }
    if (*bi + 2 > block_argc) return -1;
    const char *key = block_argv[*bi]; (*bi)++;
    const char *envvar = block_argv[*bi]; (*bi)++;

    const char *envval = getenv(envvar);
    if (!envval) {
        if (insist) {
            fprintf(stderr, "multisubstitute: importas: variable %s not set\n", envvar);
            return -1;
        }
        if (opt_D) envval = opt_D;
    }

    if (*nsubst >= *subst_cap) {
        *subst_cap *= 2;
        *substs = xrealloc(*substs, sizeof(struct subst) * *subst_cap);
    }
    if (split && delim == ' ') delim = backtick_hint_delim(envvar);
    (*substs)[*nsubst].key = xstrdup(key);
    (*substs)[*nsubst].value = envval ? xstrdup(envval) : NULL;
    (*substs)[*nsubst].split = split;
    (*substs)[*nsubst].crunch = 0;
    (*substs)[*nsubst].chomp = 0;
    (*substs)[*nsubst].delim = delim;
    (*nsubst)++;
    return 0;
}

/* Parse an "elglob" subcommand inside multisubstitute block */
static int
parse_ms_elglob(char *block_argv[], int block_argc,
                int *bi, struct subst **substs,
                int *nsubst, int *subst_cap) {
    bool split = false;
    char delim = ' ';
    bool opt_m = false;
    bool opt_0 = false;
    while (*bi < block_argc && is_opt(block_argv[*bi])) {
        if (eq(block_argv[*bi], "-v")) { (*bi)++; }
        else if (eq(block_argv[*bi], "-w")) { (*bi)++; }
        else if (eq(block_argv[*bi], "-s")) { split = true; (*bi)++; }
        else if (eq(block_argv[*bi], "-m")) { opt_m = true; (*bi)++; }
        else if (eq(block_argv[*bi], "-e")) { (*bi)++; }
        else if (eq(block_argv[*bi], "-0")) { opt_0 = true; (*bi)++; }
        else break;
    }
    if (*bi + 2 > block_argc) return -1;
    const char *key = block_argv[*bi]; (*bi)++;
    const char *pattern = block_argv[*bi]; (*bi)++;

    glob_t g = {0};
    int flags = 0;
    if (opt_m) flags |= GLOB_NOCHECK;
    if (glob(pattern, flags, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        if (*nsubst >= *subst_cap) {
            *subst_cap *= 2;
            *substs = xrealloc(*substs, sizeof(struct subst) * *subst_cap);
        }
        (*substs)[*nsubst].key = xstrdup(key);
        (*substs)[*nsubst].value = NULL;
        (*substs)[*nsubst].split = false;
        (*substs)[*nsubst].crunch = 0;
        (*substs)[*nsubst].chomp = 0;
        (*substs)[*nsubst].delim = ' ';
        (*nsubst)++;
        return 0;
    }

    char *value = glob_join(&g);
    globfree(&g);

    if (*nsubst >= *subst_cap) {
        *subst_cap *= 2;
        *substs = xrealloc(*substs, sizeof(struct subst) * *subst_cap);
    }
    if (split) {
        if (opt_0) {
            char hint[64];
            snprintf(hint, sizeof(hint), "EXECLINE_DELIM0_%s", key);
            setenv(hint, "1", 1);
        }
    }
    (*substs)[*nsubst].key = xstrdup(key);
    (*substs)[*nsubst].value = value;
    (*substs)[*nsubst].split = split;
    (*substs)[*nsubst].crunch = 0;
    (*substs)[*nsubst].chomp = 0;
    (*substs)[*nsubst].delim = delim;
    (*nsubst)++;
    return 0;
}

/* --- multisubstitute --- */
static int cmd_multisubstitute(int argc, char *argv[]) {
    /* multisubstitute { define ... importas ... elglob ... } prog... */
    int i = 1;

    int block_argc;
    char **block_argv;
    int block_end;
    read_block(argc, argv, i, &block_argc, &block_argv, &block_end);

    int nsubst = 0;
    int subst_cap = 8;
    struct subst *substs = xmalloc(sizeof(struct subst) * subst_cap);

    int bi = 0;
    while (bi < block_argc) {
        const char *cmd = block_argv[bi]; bi++;
        if (eq(cmd, "define")) {
            if (parse_ms_define(block_argv, block_argc, &bi, &substs, &nsubst, &subst_cap) < 0) break;
        } else if (eq(cmd, "importas")) {
            if (parse_ms_importas(block_argv, block_argc, &bi, &substs, &nsubst, &subst_cap) < 0) break;
        } else if (eq(cmd, "elglob")) {
            if (parse_ms_elglob(block_argv, block_argc, &bi, &substs, &nsubst, &subst_cap) < 0) break;
        } else break;
    }

    free_block(block_argc, block_argv);

    int rest_argc = argc - block_end;
    if (rest_argc <= 0) { free(substs); return 0; }

    int new_argc;
    char **new_argv = subst_argv(rest_argc, argv + block_end, substs, nsubst, &new_argc);
    if (new_argc == -1) {
        for (int si = 0; si < nsubst; si++) { free((void*)substs[si].key); free((void*)substs[si].value); }
        free(substs);
        return 111;
    }
    for (int si = 0; si < nsubst; si++) {
        free((void*)substs[si].key);
        free((void*)substs[si].value);
    }
    free(substs);
    int ms_result = cmd_dispatch(new_argc, new_argv);
    free_argv(new_argc, new_argv);
    return ms_result;
}

/* --- emptyenv --- */
static int cmd_emptyenv(int argc, char *argv[]) {
    /* emptyenv [ -P ] prog... */
    bool opt_P = false;
    int i = 1;

    if (i < argc && eq(argv[i], "-P")) { opt_P = true; i++; }

    if (opt_P) {
        /* Push frame: save special vars, clear, restore them.
         * Other env vars are cleaned for the child chain. */
        const char *save_vars[] = {"#", "0", "1", "2", "3", "4", "5",
                                  "6", "7", "8", "9", "?", "!", NULL};
        char *saved[sizeof(save_vars)/sizeof(save_vars[0])] = {0};
        for (int j = 0; save_vars[j]; j++) {
            const char *v = getenv(save_vars[j]);
            if (v) saved[j] = xstrdup(v);
        }

        env_clear();

        for (int j = 0; save_vars[j]; j++) {
            if (saved[j]) {
                setenv(save_vars[j], saved[j], 1);
                free(saved[j]);
            }
        }
    } else {
        env_clear();
    }

    return cmd_dispatch(argc - i, argv + i);
}

/* --- dollarat --- */
static int cmd_dollarat(int argc, char *argv[]) {
    /* dollarat [ -n ] [ -0 | -d delimchar ] */
    bool opt_n = false;
    char delim = ' ';
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-n")) { opt_n = true; i++; }
        else if (eq(argv[i], "-0")) { delim = '\0'; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else break;
    }

    /* Read # env var for count */
    const char *numstr = getenv("#");
    if (!numstr) return 100;

    bool ok;
    int n = parse_int(numstr, &ok);
    if (!ok) return 100;

    int first = 1;
    for (int j = 1; j <= n; j++) {
        char varname[16];
        snprintf(varname, sizeof(varname), "%d", j);
        const char *val = getenv(varname);
        if (!val) continue;

        if (!first) fwrite(&delim, 1, 1, stdout);
        first = 0;
        fputs(val, stdout);
    }

    if (!opt_n) fputc('\n', stdout);

    /* No chain - dollarat just prints */
    return 0;
}

/* --- eltest --- */
/* Inline POSIX test(1) — no fork, no exec. */

/* Evaluate test primary at *pos. Returns 0=false, 1=true, -1=syntax. */
static int
test_primary(const char **argv, int argc, int *pos) {
    if (*pos >= argc) return -1;

    const char *op = argv[(*pos)++];

    /* Parenthesized sub-expression */
    if (eq(op, "(")) {
        int r = test_primary(argv, argc, pos);
        if (r < 0) return -1;
        /* Parse -a / -o within parens */
        while (*pos < argc && !eq(argv[*pos], ")")) {
            const char *binop = argv[(*pos)++];
            if (*pos >= argc) return -1;
            if (eq(binop, "-a")) {
                int right = test_primary(argv, argc, pos);
                if (right < 0) return -1;
                r = r && right;
            } else if (eq(binop, "-o")) {
                int right = test_primary(argv, argc, pos);
                if (right < 0) return -1;
                r = r || right;
            } else return -1;
        }
        if (*pos >= argc || !eq(argv[*pos], ")")) return -1;
        (*pos)++;
        return r;
    }

    /* Negation */
    if (eq(op, "!")) {
        int r = test_primary(argv, argc, pos);
        return (r < 0) ? -1 : !r;
    }

    /* Unary operators */
    if (op[0] == '-' && op[1] && !op[2]) {
        /* One-character unary: -b -c -d -e -f -g -L -p -r -S -s -u -w -x -z -n */
        if (*pos >= argc) return -1;
        const char *operand = argv[(*pos)++];
        struct stat st;
        switch (op[1]) {
            case 'b': return stat(operand, &st) == 0 && S_ISBLK(st.st_mode);
            case 'c': return stat(operand, &st) == 0 && S_ISCHR(st.st_mode);
            case 'd': return stat(operand, &st) == 0 && S_ISDIR(st.st_mode);
            case 'e': return access(operand, F_OK) == 0;
            case 'f': return stat(operand, &st) == 0 && S_ISREG(st.st_mode);
            case 'g': return stat(operand, &st) == 0 && (st.st_mode & S_ISGID);
            case 'L': return lstat(operand, &st) == 0 && S_ISLNK(st.st_mode);
            case 'p': return stat(operand, &st) == 0 && S_ISFIFO(st.st_mode);
            case 'r': return access(operand, R_OK) == 0;
            case 'S': return stat(operand, &st) == 0 && S_ISSOCK(st.st_mode);
            case 's': { struct stat st2; return stat(operand, &st2) == 0 && st2.st_size > 0; }
            case 'u': return stat(operand, &st) == 0 && (st.st_mode & S_ISUID);
            case 'w': return access(operand, W_OK) == 0;
            case 'x': return access(operand, X_OK) == 0;
            case 'z': return is_empty(operand);
            case 'n': return !is_empty(operand);
        }
        return -1;
    }

    /* -t [fd]: is fd a terminal? */
    if (eq(op, "-t")) {
        int fd = 1;
        if (*pos < argc && argv[*pos][0] != '-' && !eq(argv[*pos], "!")) {
            bool ok;
            fd = parse_int(argv[(*pos)++], &ok);
            if (!ok) return -1;
        }
        return isatty(fd);
    }

    /* Binary operators — need two more args */
    if (*pos >= argc) {
        /* Single arg: true if non-empty */
        return !is_empty(op);
    }

    const char *binop = argv[(*pos)++];
    if (*pos >= argc) return -1;
    const char *rhs = argv[(*pos)++];

    if (eq(binop, "="))   return eq(op, rhs);
    if (eq(binop, "!="))  return !eq(op, rhs);
    if (eq(binop, "-eq")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a == b; }
    if (eq(binop, "-ne")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a != b; }
    if (eq(binop, "-gt")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a > b; }
    if (eq(binop, "-ge")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a >= b; }
    if (eq(binop, "-lt")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a < b; }
    if (eq(binop, "-le")) { bool aok, bok; long a = parse_long(op, &aok); long b = parse_long(rhs, &bok); return aok && bok && a <= b; }
    if (eq(binop, "-nt")) { struct stat a_st, b_st; return stat(op, &a_st) == 0 && stat(rhs, &b_st) == 0 && a_st.st_mtime > b_st.st_mtime; }
    if (eq(binop, "-ot")) { struct stat a_st, b_st; return stat(op, &a_st) == 0 && stat(rhs, &b_st) == 0 && a_st.st_mtime < b_st.st_mtime; }
    if (eq(binop, "-ef")) { struct stat a_st, b_st; return stat(op, &a_st) == 0 && stat(rhs, &b_st) == 0 && a_st.st_dev == b_st.st_dev && a_st.st_ino == b_st.st_ino; }

    /* String comparison operators (locale-sensitive in POSIX) */
    if (eq(binop, "<"))  return strcmp(op, rhs) < 0;
    if (eq(binop, ">"))  return strcmp(op, rhs) > 0;

    return -1;
}

/* Evaluate a test expression with -a / -o precedence. */
static int
test_expr(const char **argv, int argc) {
    int pos = 1;
    int r = test_primary(argv, argc, &pos);
    if (r < 0) return 1;

    /* -a (and) has higher precedence than -o (or).
     * Parse AND chain first, then OR chain. */
    /* Collect AND terms */
    int values[64];
    int nvals = 0;
    values[nvals++] = r;

    while (pos < argc) {
        if (eq(argv[pos], "-a")) {
            pos++;
            int right = test_primary(argv, argc, &pos);
            if (right < 0) return 1;
            values[nvals-1] = values[nvals-1] && right;
        } else if (eq(argv[pos], "-o")) {
            pos++;
            int right = test_primary(argv, argc, &pos);
            if (right < 0) return 1;
            values[nvals++] = right;
        } else break;
    }

    for (int i = 0; i < nvals; i++)
        if (values[i]) return 0;
    return 1;
}

static int
cmd_eltest(int argc, char *argv[]) {
    if (argc < 2) return 1;
    return test_expr((const char **)argv, argc);
}

/* --- forx --- */
static int cmd_forx(int argc, char *argv[]) {
    /* forx [ -p | -o codes | -x codes ] [ -E | -e ] [ -C | -c ] var { values... } loop... */
    bool opt_c = false;
    unsigned char opt_o_map[32] = {0};
    unsigned char opt_x_map[32] = {0};
    bool has_opt_o = false, has_opt_x = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-p") || eq(argv[i], "-E") || eq(argv[i], "-e")) { i++; }
        else if (eq(argv[i], "-o") && i+1 < argc) { parse_code_map(argv[i+1], opt_o_map); has_opt_o = true; i += 2; }
        else if (eq(argv[i], "-x") && i+1 < argc) { parse_code_map(argv[i+1], opt_x_map); has_opt_x = true; i += 2; }
        else if (eq(argv[i], "-c")) { opt_c = true; i++; }
        else if (eq(argv[i], "-C")) { i++; }
        else if (eq(argv[i], "--")) { i++; break; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "forx: usage: forx var { values... } loop...\n");
        return 100;
    }

    const char *var = argv[i]; i++;

    int values_argc;
    char **values_argv;
    int values_end;
    read_block(argc, argv, i, &values_argc, &values_argv, &values_end);

    int loop_start = values_end;
    int last_status = 0;

    for (int j = 0; j < values_argc; j++) {
        setenv(var, values_argv[j], 1);

        if (loop_start < argc) {
            int status = run_block(argc - loop_start, argv + loop_start);
            last_status = status;
            if (has_opt_x && in_code_map(opt_x_map, status)) break;
            if (has_opt_o && !in_code_map(opt_o_map, status)) break;
            if (!has_opt_o && !has_opt_x && status != 0 && !opt_c) break;
        }
    }

    free_block(values_argc, values_argv);
    return last_status;
}

/* Read one record from stdin. Returns 0 on EOF. Reallocs *line. */
static int
read_record(char **line, size_t *cap, char delim) {
    size_t pos = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (delim == '\0') {
            if (c == '\0') break;
        } else {
            if (c == delim) break;
            if (delim == '\n' && c == '\r') {
                int next = fgetc(stdin);
                if (next != '\n' && next != EOF) ungetc(next, stdin);
                break;
            }
        }
        if (pos + 1 >= *cap) { *cap *= 2; *line = xrealloc(*line, *cap); }
        (*line)[pos++] = c;
    }
    if (c == EOF && pos == 0) return 0;
    (*line)[pos] = '\0';
    return 1;
}

/* Strip leading/trailing whitespace, optionally squeeze internal sequences.
 * Returns start of stripped string (may differ from input pointer). */
static char *
strip_whitespace(char *s, int squeeze) {
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (squeeze) {
        char *wp = start;
        int was_space = 0;
        for (char *rp = start; rp < end; rp++) {
            if (*rp == ' ' || *rp == '\t' || *rp == '\r') {
                if (!was_space) { *wp++ = ' '; was_space = 1; }
            } else {
                *wp++ = *rp;
                was_space = 0;
            }
        }
        end = wp;
    }
    *end = '\0';
    return start;
}

/* --- forstdin --- */
static int cmd_forstdin(int argc, char *argv[]) {
    /* forstdin [ -p | -o codes | -x codes ] [ -E | -e ] [ -w | -W ] [ -0 | -d delim ] var loop... */
    int i = 1;
    bool opt_c = false;
    char delim = '\n';
    unsigned char opt_o_map[32] = {0};
    unsigned char opt_x_map[32] = {0};
    bool has_opt_o = false, has_opt_x = false;
    bool opt_w = false;  /* -w: strip leading/trailing whitespace */
    bool opt_W = false;  /* -W: also squeeze internal whitespace */

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-0")) { delim = '\0'; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) {
            delim = argv[i+1][0]; i += 2;
        }
        else if (eq(argv[i], "-E") || eq(argv[i], "-e")) { i++; }
        else if (eq(argv[i], "-p")) { i++; }
        else if (eq(argv[i], "-o") && i+1 < argc) { parse_code_map(argv[i+1], opt_o_map); has_opt_o = true; i += 2; }
        else if (eq(argv[i], "-x") && i+1 < argc) { parse_code_map(argv[i+1], opt_x_map); has_opt_x = true; i += 2; }
        else if (eq(argv[i], "-w")) { opt_w = true; i++; }
        else if (eq(argv[i], "-W")) { opt_W = true; opt_w = true; i++; }
        else if (eq(argv[i], "-c")) { opt_c = true; i++; }
        else if (eq(argv[i], "-C")) { i++; }
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "forstdin: usage: forstdin var loop...\n");
        return 100;
    }

    const char *var = argv[i]; i++;
    int loop_start = i;
    int last_status = 0;

    size_t buf_cap = 4096;
    char *line = xmalloc(buf_cap);

    while (read_record(&line, &buf_cap, delim)) {

        if (opt_w) {
            char *val = strip_whitespace(line, opt_W);
            setenv(var, val, 1);
        } else {
            setenv(var, line, 1);
        }

        if (loop_start < argc) {
            int status = run_block(argc - loop_start, argv + loop_start);
            last_status = status;
            if (has_opt_x && in_code_map(opt_x_map, status)) break;
            if (has_opt_o && !in_code_map(opt_o_map, status)) break;
            if (!has_opt_o && !has_opt_x && status != 0 && !opt_c) break;
        }
    }

    free(line);
    return last_status;
}

/* ------------------------------------------------------------------ */
/* --- execlineb ---                                                   */
/* ------------------------------------------------------------------ */

/*
 * Parse execline script text into argv format.
 * Handles { } blocks, "" quoted strings, \" escaping, # comments.
 * Blocks are emitted as space-prefixed args per nesting depth,
 * terminated by an empty arg.
 */

/* Push a word onto the dynamic argv builder, prefixing with n spaces */
static void
argv_emit(char ***argv, int *n, int *cap, const char *word, int depth) {
    size_t wlen = strlen(word);
    size_t total = depth + wlen + 1;
    char *s = xmalloc(total);
    memset(s, ' ', depth);
    memcpy(s + depth, word, wlen + 1);
    if (*n >= *cap) { *cap *= 2; *argv = xrealloc(*argv, sizeof(char *) * *cap); }
    (*argv)[(*n)++] = s;
}

/* Emit a block terminator (empty arg, space-prefixed per depth) */
static void
argv_term(char ***argv, int *n, int *cap, int depth) {
    size_t sz = depth + 1;
    char *s = xmalloc(sz);
    memset(s, ' ', depth);
    s[depth] = '\0';
    if (*n >= *cap) { *cap *= 2; *argv = xrealloc(*argv, sizeof(char *) * *cap); }
    (*argv)[(*n)++] = s;
}

/*
 * Parse execline script string into argv.
 * Iterative approach: tracks nesting depth via { / }.
 * At block boundaries, emits block terminator '' and adjusts depth.
 * Returns 0 on success, -1 on syntax error.
 */
static int
parse_script(const char *s, char ***argv, int *n, int *cap) {
    int depth = 0;

    while (*s) {
        /* Skip whitespace */
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
            s++;
        if (!*s) break;

        /* Skip line comments */
        if (*s == '#') {
            while (*s && *s != '\n') s++;
            continue;
        }

        /* Block open */
        if (*s == '{') {
            s++;
            depth++;
            continue;
        }

        /* Block close */
        if (*s == '}') {
            s++;
            if (depth == 0) return -1;  /* unmatched } */
            depth--;
            argv_term(argv, n, cap, depth);  /* terminate block at new depth */
            continue;
        }

        /* Quoted string */
        if (*s == '"') {
            int wcap = 64, wlen = 0;
            char *word = xmalloc(wcap);
            s++;  /* skip opening quote */
            while (*s && *s != '"') {
                if (*s == '\\' && *(s+1)) {
                    s++;
                    if (*s == '"') {
                        if (wlen + 1 >= wcap) { wcap *= 2; word = xrealloc(word, wcap); }
                        word[wlen++] = '"';
                        s++;
                        continue;
                    }
                    /* Other escapes: consume backslash, keep escaped char.
                     * Fall through to add *s (the char after backslash). */
                }
                if (wlen + 1 >= wcap) { wcap *= 2; word = xrealloc(word, wcap); }
                word[wlen++] = *s;
                s++;
            }
            if (!*s) { free(word); return -1; }
            s++;  /* skip closing quote */
            word[wlen] = '\0';
            argv_emit(argv, n, cap, word, depth);
            free(word);
            continue;
        }

        /* Unquoted word: collect until whitespace, }, or end */
        int wcap = 64, wlen = 0;
        char *word = xmalloc(wcap);
        while (*s && *s != ' ' && *s != '\t' && *s != '\n' &&
               *s != '\r' && *s != '}' && *s != '#') {
            if (*s == '\\' && *(s+1)) {
                s++;
            }
            if (wlen + 1 >= wcap) { wcap *= 2; word = xrealloc(word, wcap); }
            word[wlen++] = *s;
            s++;
        }
        word[wlen] = '\0';
        argv_emit(argv, n, cap, word, depth);
        free(word);
    }

    return depth == 0 ? 0 : -1;  /* unmatched { */
}

/* Count positional params from env var # */
static int
get_positionals_count(void) {
    const char *s = getenv("#");
    if (!s) return 0;
    bool ok;
    int n = parse_int(s, &ok);
    return ok ? n : 0;
}

/* Read a file into an allocated string (caller must free) */
static char *
read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc(fsize + 1);
    size_t nread = fread(buf, 1, fsize, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

/* Set env vars #, $1..$N from argv[offset..offset+N-1] */
static void
set_positional(int n, char *argv[], int offset) {
    char numstr[16];
    snprintf(numstr, sizeof(numstr), "%d", n);
    setenv("#", numstr, 1);
    for (int j = 1; j <= n; j++) {
        char varname[16];
        snprintf(varname, sizeof(varname), "%d", j);
        setenv(varname, argv[offset + j - 1], 1);
    }
}

/* Replace $@ in parsed argv with positional values from env */
static void
expand_dollarat(char ***parsed, int *argv_n, int *argv_cap) {
    int npos = get_positionals_count();
    if (npos <= 0) return;

    int ai = 0;
    while (ai < *argv_n) {
        if (eq((*parsed)[ai], "$@")) {
            int new_n = *argv_n - 1 + npos;
            if (new_n > *argv_cap) {
                *argv_cap = new_n + 8;
                *parsed = xrealloc(*parsed, sizeof(char *) * *argv_cap);
            }
            free((*parsed)[ai]);
            memmove(*parsed + ai + npos, *parsed + ai + 1,
                    sizeof(char *) * (*argv_n - ai - 1));
            for (int j = 1; j <= npos; j++) {
                char vn[16];
                snprintf(vn, sizeof(vn), "%d", j);
                const char *v = getenv(vn);
                (*parsed)[ai + j - 1] = xstrdup(v ? v : "");
            }
            *argv_n = new_n;
            ai += npos;
        } else {
            ai++;
        }
    }
}

static int
cmd_execlineb(int argc, char *argv[]) {
    /* execlineb [ -q | -w | -W ] [ -p | -P | -S nmin | -s nmin ]
     *          -c script [args...]
     * or: execlineb [opts] scriptfile [args...] */
    const char *script = NULL;
    const char *scriptfile = NULL;
    int script_args = 0;
    bool opt_p = false;
    bool opt_P = false;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-c") && i+1 < argc) {
            script = argv[i+1];
            script_args = i + 2;
            i += 2;
        } else if (eq(argv[i], "-q")) {
            setenv("EXECLINE_STRICT", "", 1); i++;
        } else if (eq(argv[i], "-w")) {
            setenv("EXECLINE_STRICT", "1", 1); i++;
        } else if (eq(argv[i], "-W")) {
            setenv("EXECLINE_STRICT", "2", 1); i++;
        } else if (eq(argv[i], "-p")) {
            opt_p = true; i++;
        } else if (eq(argv[i], "-P")) {
            opt_P = true; i++;
        } else if (has_prefix(argv[i], "-S") || has_prefix(argv[i], "-s")) {
            const char *val = argv[i] + 2;
            if (*val) {
                bool ok;
                parse_int(val, &ok);
                if (!ok) { fprintf(stderr, "execlineb: invalid -S/-s argument\n"); return 100; }
                i++;
            } else if (i+1 < argc) {
                bool ok;
                parse_int(argv[i+1], &ok);
                if (!ok) { fprintf(stderr, "execlineb: invalid -S/-s argument\n"); return 100; }
                i += 2;
            } else {
                fprintf(stderr, "execlineb: -S/-s requires an argument\n"); return 100;
            }
        } else {
            break;
        }
    }

    if (!script) {
        if (i >= argc) {
            fprintf(stderr, "execlineb: usage: execlineb -c script [args...]\n");
            return 100;
        }
        scriptfile = argv[i];
        script_args = i + 1;
        script = read_file(scriptfile);
        if (!script) {
            fprintf(stderr, "execlineb: unable to open %s\n", scriptfile);
            return 111;
        }
    }

    setenv("0", scriptfile ? scriptfile : "-c", 1);
    if (!opt_P)
        set_positional(argc - script_args, argv, script_args);

    int argv_cap = 64, argv_n = 0;
    char **parsed = xmalloc(sizeof(char *) * argv_cap);

    if (parse_script(script, &parsed, &argv_n, &argv_cap) < 0) {
        fprintf(stderr, "execlineb: syntax error\n");
        return 100;
    }

    if (argv_n == 0) { free(parsed); return 0; }

    if (!opt_p)
        expand_dollarat(&parsed, &argv_n, &argv_cap);

    int result = cmd_dispatch(argv_n, parsed);
    /* parsed argv not NULL-terminated — free_argv frees by count */
    free_argv(argv_n, parsed);
    return result;
}

/* ------------------------------------------------------------------ */
/* --- elgetpositionals ---                                             */
/* ------------------------------------------------------------------ */

/* Collect positional values from $start..$end into delim-separated string */
static char *
collect_positionals(int start, int end, char delim) {
    size_t cap = 64, len = 0;
    char *value = xmalloc(cap);
    value[0] = '\0';
    for (int j = start; j <= end; j++) {
        char vn[16];
        snprintf(vn, sizeof(vn), "%d", j);
        const char *v = getenv(vn);
        if (!v) continue;
        if (len > 0) {
            size_t need = len + 1 + strlen(v) + 1;
            if (need > cap) { cap = need; value = xrealloc(value, cap); }
            value[len++] = delim;
            value[len] = '\0';
        }
        size_t need = len + strlen(v) + 1;
        if (need > cap) { cap = need; value = xrealloc(value, cap); }
        memcpy(value + len, v, strlen(v) + 1);
        len += strlen(v);
    }
    return value;
}

static int
cmd_elgetpositionals(int argc, char *argv[]) {
    /* elgetpositionals [ -s ] [ -d delim ] [ -N | -n ] [ -C | -c ]
     *                   [ -D default | -i ] var shift prog... */
    bool opt_s = false;
    char delim = ' ';
    const char *opt_D = NULL;
    bool opt_i = false;
    int shift = 0;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else if (eq(argv[i], "-n") || eq(argv[i], "-N")) { i++; }
        else if (eq(argv[i], "-c") || eq(argv[i], "-C")) { i++; }
        else if (eq(argv[i], "-i")) { opt_i = true; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else if (eq(argv[i], "-D") && i+1 < argc) { opt_D = argv[i+1]; i += 2; }
        else break;
    }

    if (i + 2 > argc) {
        fprintf(stderr, "elgetpositionals: usage: var shift prog...\n");
        return 100;
    }

    const char *var = argv[i]; i++;
    {
        bool ok;
        shift = parse_int(argv[i], &ok);
        if (!ok) { fprintf(stderr, "elgetpositionals: invalid shift\n"); return 100; }
    }
    i++;
    int prog_start = i;
    int npos = get_positionals_count();
    int nprog = argc - prog_start;

    if (shift >= npos && !opt_D) {
        if (opt_i) {
            fprintf(stderr, "elgetpositionals: not enough positional params\n");
            return 100;
        }
        if (nprog > 0) {
            struct subst sub;
            sub.key = var;
            sub.value = NULL;
            sub.split = opt_s;
            sub.crunch = 0;
            sub.chomp = 0;
            sub.delim = delim;
            int na; char **nv = subst_argv(nprog, argv+prog_start, &sub, 1, &na);
            if (na == -1) return 111;
            int r = cmd_dispatch(na, nv);
            free_argv(na, nv);
            return r;
        }
        return 0;
    }

    char *value = collect_positionals(shift + 1, npos, delim);
    if (opt_D && *value == '\0') {
        free(value);
        value = xstrdup(opt_D);
    }

    if (prog_start < argc) {
        struct subst sub;
        sub.key = var;
        sub.value = value;
        sub.split = opt_s;
        sub.crunch = 0;
        sub.chomp = 0;
        sub.delim = delim;
        int na; char **nv = subst_argv(nprog, argv+prog_start, &sub, 1, &na);
        free(value);
        if (na == -1) return 111;
        int r = cmd_dispatch(na, nv);
        free_argv(na, nv);
        return r;
    }
    free(value);
    return 0;
}

/* ------------------------------------------------------------------ */
/* --- heredoc ---                                                      */
/* ------------------------------------------------------------------ */

static int
cmd_heredoc(int argc, char *argv[]) {
    /* heredoc [ -d ] fd string prog...
     * Skarnet-compatible: pipe-based, no temp file. */
    bool opt_d = false;
    int i = 1;
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "--")) { i++; break; }
        if (eq(argv[i], "-d")) { opt_d = true; i++; }
        else break;
    }

    if (i + 2 >= argc) {
        fprintf(stderr, "heredoc: usage: heredoc [ -d ] fd string prog...\n");
        return 100;
    }

    bool ok;
    int target_fd = parse_int(argv[i], &ok);
    if (!ok || target_fd < 0) {
        fprintf(stderr, "heredoc: invalid fd: %s\n", argv[i]);
        return 100;
    }
    const char *data = argv[i + 1];
    int prog_start = i + 2;

    int pfd[2];
    if (pipe(pfd) < 0) die_sys();

    pid_t pid = fork();
    if (pid < 0) die_sys();
    if (pid == 0) {
        if (opt_d) {
            pid_t pid2 = fork();
            if (pid2 < 0) _exit(111);
            if (pid2 > 0) _exit(0);
        }
        close(pfd[0]);
        write_all(pfd[1], data, strlen(data));
        close(pfd[1]);
        _exit(0);
    }

    close(pfd[1]);
    if (dup2(pfd[0], target_fd) < 0) {
        fprintf(stderr, "heredoc: dup2: %s\n", strerror(errno));
        close(pfd[0]);
        return 111;
    }
    close(pfd[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);
    set_status_var(waitstatus_exit_code(wstatus));

    return cmd_dispatch(argc - prog_start, argv + prog_start);
}

/* ------------------------------------------------------------------ */
/* --- getcwd ---                                                       */
/* ------------------------------------------------------------------ */

static int
cmd_getcwd(int argc, char *argv[]) {
    /* getcwd [ -E | -e ] var prog... */
    bool do_export = true;
    int i = 1;
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-E")) { do_export = true; i++; }
        else if (eq(argv[i], "-e")) { do_export = false; i++; }
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "getcwd: usage: getcwd var prog...\n");
        return 100;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "getcwd: %s\n", strerror(errno));
        return 111;
    }
    if (do_export) setenv(argv[i], cwd, 1);
    return cmd_dispatch(argc - i - 1, argv + i + 1);
}

/* ------------------------------------------------------------------ */
/* --- fdmove ---                                                       */
/* ------------------------------------------------------------------ */

static int
cmd_fdmove(int argc, char *argv[]) {
    /* fdmove [ -c ] oldfd newfd prog... */
    bool opt_c = false;
    int i = 1;
    if (i < argc && eq(argv[i], "-c")) { opt_c = true; i++; }
    if (i + 2 > argc) {
        fprintf(stderr, "fdmove: usage: fdmove oldfd newfd prog...\n");
        return 100;
    }
    bool ok;
    int oldfd = parse_int(argv[i], &ok);
    if (!ok) { fprintf(stderr, "fdmove: invalid oldfd\n"); return 100; }
    int newfd = parse_int(argv[i+1], &ok);
    if (!ok) { fprintf(stderr, "fdmove: invalid newfd\n"); return 100; }
    if (dup2(oldfd, newfd) < 0) { die_sys(); }
    if (opt_c) close(oldfd);
    return cmd_dispatch(argc - i - 2, argv + i + 2);
}

/* ------------------------------------------------------------------ */
/* --- multidefine ---                                                  */
/* ------------------------------------------------------------------ */

static int
cmd_multidefine(int argc, char *argv[]) {
    /* multidefine [ -s ] [ -d delim ] key1 val1 ... -- prog... */
    /* Pairs end at '--', or at first arg containing '/' or starting with '-' */
    bool opt_s = false;
    char delim = ' ';
    int i = 1;
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-s")) { opt_s = true; i++; }
        else if (eq(argv[i], "-d") && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else break;
    }
    /* Collect pairs until separator or non-identifier */
    int subst_cap = 16;
    struct subst *substs = xmalloc(sizeof(struct subst) * subst_cap);
    int nsubst = 0;
    int pi = i;
    while (pi + 1 < argc) {
        if (eq(argv[pi], "--")) { pi++; break; }
        if (strchr(argv[pi], '/') || is_opt(argv[pi]) || !*argv[pi]) break;
        if (nsubst >= subst_cap) { subst_cap *= 2; substs = xrealloc(substs, sizeof(struct subst) * subst_cap); }
        substs[nsubst].key = xstrdup(argv[pi]);
        substs[nsubst].value = xstrdup(argv[pi+1]);
        substs[nsubst].split = opt_s;
        substs[nsubst].crunch = 0;
        substs[nsubst].chomp = 0;
        substs[nsubst].delim = delim;
        nsubst++;
        pi += 2;
    }
    int prog_start = pi;
    int nprog = argc - prog_start;
    if (nprog <= 0 || nsubst == 0) {
        for (int j = 0; j < nsubst; j++) { free((void*)substs[j].key); free((void*)substs[j].value); }
        free(substs);
        if (nprog > 0) return cmd_dispatch(nprog, argv + prog_start);
        return 0;
    }
    int na; char **nv = subst_argv(nprog, argv + prog_start, substs, nsubst, &na);
    for (int j = 0; j < nsubst; j++) { free((void*)substs[j].key); free((void*)substs[j].value); }
    free(substs);
    if (na == -1) return 111;
    int md_result = cmd_dispatch(na, nv);
    free_argv(na, nv);
    return md_result;
}

/* ------------------------------------------------------------------ */
/* --- empty ---                                                        */
/* ------------------------------------------------------------------ */

static int
cmd_empty(int argc, char *argv[]) {
    /* empty [ -P ] var... prog... */
    /* Var names are consumed until '--', '/', or '-'. Prog is the rest.
     * Without a separator, prog must contain '/' or '-'. */
    bool opt_P = false;
    int i = 1;
    if (i < argc && eq(argv[i], "-P")) { opt_P = true; i++; }
    if (opt_P) {
        unsetenv("#");
        for (int j = 0; j <= 9; j++) {
            char vn[4]; snprintf(vn, sizeof(vn), "%d", j);
            unsetenv(vn);
        }
        unsetenv("?");
        unsetenv("!");
    } else {
        while (i < argc) {
            if (eq(argv[i], "--")) { i++; break; }
            if (strchr(argv[i], '/') || strchr(argv[i], '=')) break;
            if (is_opt(argv[i])) break;
            unsetenv(argv[i]);
            i++;
        }
    }
    return cmd_dispatch(argc - i, argv + i);
}

/* ------------------------------------------------------------------ */
/* --- envfile ---                                                      */
/* ------------------------------------------------------------------ */

/* Parse envfile lines into setenv calls */
static void
parse_env_lines(FILE *f, int opt_n, int opt_i, int opt_I) {
    char buf[65536];
    size_t pos = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\\') {
            int next = fgetc(f);
            if (next == '\n') continue;
            if (next != EOF) {
                if (pos < sizeof(buf) - 1) buf[pos++] = '\\';
                if (pos < sizeof(buf) - 1) buf[pos++] = next;
            }
        } else {
            if (pos < sizeof(buf) - 1) buf[pos++] = ch;
        }
    }
    buf[pos] = '\0';

    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl++ = '\0'; else nl = NULL;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') { line = nl; continue; }

        char in_q = 0;
        char *p = line;
        while (*p) {
            if (*p == '"' && in_q != '\'') in_q = in_q ? 0 : '"';
            else if (*p == '\'' && in_q != '"') in_q = in_q ? 0 : '\'';
            else if (*p == '#' && !in_q) { *p = '\0'; break; }
            p++;
        }

        char *eqp = strchr(line, '=');
        if (eqp) {
            *eqp = '\0';
            char *val = eqp + 1;
            char *k = line + strlen(line);
            while (k > line && (k[-1] == ' ' || k[-1] == '\t')) k--;
            *k = '\0';
            size_t vlen = strlen(val);
            if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                              (val[0] == '\'' && val[vlen-1] == '\''))) {
                val[vlen-1] = '\0';
                val++;
            }
            if (*line && !opt_n) setenv(line, val, 1);
        } else if (!opt_I) {
            fprintf(stderr, "envfile: ignoring malformed line: %s\n", line);
        }
        line = nl;
    }
}

/* Open an envfile and parse it. Returns 0 on success, 111 on fatal open error. */
static int
read_envfile(const char *path, int opt_n, int opt_i, int opt_I) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (opt_i) {
            fprintf(stderr, "envfile: unable to open %s: %s\n", path, strerror(errno));
            return 111;
        }
        if (!opt_I) {
            fprintf(stderr, "envfile: unable to open %s: %s\n", path, strerror(errno));
            return 111;
        }
        return 0;
    }
    parse_env_lines(f, opt_n, opt_i, opt_I);
    fclose(f);
    return 0;
}

static int
cmd_envfile(int argc, char *argv[]) {
    /* envfile [ -i | -I ] [ file ] prog...
     * -i  error if file missing (default)
     * -I  silently skip if file missing
     * If file is "-", read from stdin. */
    bool opt_i = true;
    bool opt_I = false;
    int opt_n = 0;
    const char *filepath = NULL;
    int i = 1;

    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-i")) { opt_i = true; opt_I = false; i++; }
        else if (eq(argv[i], "-I")) { opt_I = true; opt_i = false; i++; }
        else if (eq(argv[i], "-n")) { opt_n = 1; i++; }
        else break;
    }

    if (i < argc && !is_opt(argv[i]) && strchr(argv[i], '=') == NULL) {
        filepath = argv[i]; i++;
    }

    if (!filepath) filepath = "env";

    int err;
    if (eq(filepath, "-")) {
        parse_env_lines(stdin, opt_n, opt_i, opt_I);
        err = 0;
    } else {
        err = read_envfile(filepath, opt_n, opt_i, opt_I);
    }
    if (err) return err;

    return cmd_dispatch(argc - i, argv + i);
}

/* ------------------------------------------------------------------ */
/* Simple fd wrappers: fdblock fdclose fdreserve fdswap               */
/* ------------------------------------------------------------------ */

static int
cmd_fdblock(int argc, char *argv[]) {
    /* fdblock fd prog... */
    bool ok; int fd = parse_int(argv[1], &ok);
    if (!ok) return 100;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return 111;
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return cmd_dispatch(argc - 2, argv + 2);
}

static int
cmd_fdclose(int argc, char *argv[]) {
    /* fdclose fd prog... */
    bool ok; int fd = parse_int(argv[1], &ok);
    if (!ok) return 100;
    close(fd);
    return cmd_dispatch(argc - 2, argv + 2);
}

static int
cmd_fdreserve(int argc, char *argv[]) {
    /* fdreserve fd prog... — ensure fd is closed, then reserve it */
    bool ok; int fd = parse_int(argv[1], &ok);
    if (!ok) return 100;
    close(fd);
    /* Reserve by opening /dev/null on the fd */
    int newfd = open("/dev/null", O_RDWR);
    if (newfd < 0) die_sys();
    if (newfd != fd) {
        dup2(newfd, fd);
        close(newfd);
    }
    return cmd_dispatch(argc - 2, argv + 2);
}

static int
cmd_fdswap(int argc, char *argv[]) {
    /* fdswap fd1 fd2 prog... */
    bool ok;
    int fd1 = parse_int(argv[1], &ok); if (!ok) return 100;
    int fd2 = parse_int(argv[2], &ok); if (!ok) return 100;
    /* Swap via intermediate */
    int tmp = fcntl(fd1, F_DUPFD, 10);
    if (tmp < 0) return 111;
    if (dup2(fd2, fd1) < 0) { close(tmp); return 111; }
    if (dup2(tmp, fd2) < 0) { close(tmp); return 111; }
    close(tmp);
    return cmd_dispatch(argc - 3, argv + 3);
}

/* ------------------------------------------------------------------ */
/* posix-cd / posix-umask — standards-compliant variants              */
/* ------------------------------------------------------------------ */

static int
cmd_posix_cd(int argc, char *argv[]) {
    /* posix-cd path prog... — plain chdir, no PWD update */
    if (argc < 2) return cmd_dispatch(0, NULL);
    if (chdir(argv[1]) < 0) {
        fprintf(stderr, "posix-cd: %s: %s\n", argv[1], strerror(errno));
        return 111;
    }
    return cmd_dispatch(argc - 2, argv + 2);
}

static int
cmd_posix_umask(int argc, char *argv[]) {
    /* posix-umask value prog... */
    if (argc < 2) { fprintf(stderr, "posix-umask: usage: value prog...\n"); return 100; }
    mode_t mask = strtol(argv[1], NULL, 8);
    umask(mask);
    return cmd_dispatch(argc - 2, argv + 2);
}

/* ------------------------------------------------------------------ */
/* --- runblock ---                                                     */
/* ------------------------------------------------------------------ */

static int
cmd_runblock(int argc, char *argv[]) {
    /* runblock [ -r ] n { block... } prog... */
    /* Runs the block with up to n args from prog prepended.
     * Default: first n args. -r: last n args (reversed order). */
    bool opt_r = false;
    int i = 1;
    if (i < argc && eq(argv[i], "-r")) { opt_r = true; i++; }
    if (i >= argc) { fprintf(stderr, "runblock: usage\n"); return 100; }
    bool ok; int n = parse_int(argv[i], &ok);
    if (!ok) { fprintf(stderr, "runblock: invalid n\n"); return 100; }
    i++;
    int block_argc; char **block_argv; int block_end;
    read_block(argc, argv, i, &block_argc, &block_argv, &block_end);

    int rest_argc = argc - block_end;
    int take = n < rest_argc ? n : rest_argc;
    int total = block_argc + take + 1;
    char **combined = xmalloc(sizeof(char *) * total);
    int ci = 0;

    /* Block args first (command and its args) */
    for (int j = 0; j < block_argc; j++)
        combined[ci++] = block_argv[j];

    /* Append up to n prog args after the block */
    if (opt_r) {
        int prog_start = block_end + (rest_argc - take);
        for (int j = 0; j < take; j++)
            combined[ci++] = argv[prog_start + j];
    } else {
        for (int j = 0; j < take; j++)
            combined[ci++] = argv[block_end + j];
    }
    combined[ci] = NULL;

    if (ci > 0) {
        int r = cmd_dispatch(ci, combined);
        free(combined);
        free_block(block_argc, block_argv);
        return r;
    }
    free(combined);
    free_block(block_argc, block_argv);
    return cmd_dispatch(rest_argc, argv + block_end);
}

/* ------------------------------------------------------------------ */
/* --- tryexec ---                                                      */
/* ------------------------------------------------------------------ */

static int
cmd_tryexec(int argc, char *argv[]) {
    /* tryexec prog... — run prog, if exec fails, continue chain */
    if (argc < 2) return 0;
    execvp(argv[1], argv + 1);
    /* If we get here, exec failed. Continue with chain after prog. */
    return cmd_dispatch(argc - 2, argv + 2);
}

/* ------------------------------------------------------------------ */
/* --- withstdinas ---                                                  */
/* ------------------------------------------------------------------ */

static int
cmd_withstdinas(int argc, char *argv[]) {
    /* withstdinas [ -r | -w ] file prog... */
    int flags = O_RDONLY;
    int i = 1;
    while (i < argc && is_opt(argv[i])) {
        if (eq(argv[i], "-r")) { flags = O_RDONLY; i++; }
        else if (eq(argv[i], "-w")) { flags = O_WRONLY|O_CREAT; i++; }
        else break;
    }
    if (i + 1 >= argc) {
        fprintf(stderr, "withstdinas: usage: file prog...\n");
        return 100;
    }
    int fd = open(argv[i], flags, 0666);
    if (fd < 0) { fprintf(stderr, "withstdinas: %s: %s\n", argv[i], strerror(errno)); return 111; }
    if (dup2(fd, 0) < 0) { close(fd); return 111; }
    close(fd);
    return cmd_dispatch(argc - i - 1, argv + i + 1);
}

/* ------------------------------------------------------------------ */
/* Main dispatch                                                      */
/* ------------------------------------------------------------------ */

/* Dispatch a command by name.
 * Returns exit code. Does not exec. */
static int
cmd_dispatch(int argc, char *argv[]) {
    if (argc < 1) return 0;

    const char *cmd = path_basename(argv[0]);

    /* "exit" */
    if (eq(cmd, "exit")) {
        if (argc >= 2) {
            bool ok;
            int code = parse_int(argv[1], &ok);
            return ok ? code : 100;
        }
        return 0;
    }

    struct cmd_entry *entry = find_cmd(cmd);
    if (entry)
        return entry->handler(argc, argv);

    /* External command */
    execvp(argv[0], argv);
    fprintf(stderr, "execline: unable to exec %s: %s\n", argv[0], strerror(errno));
    return 127;
}

int main(int argc, char *argv[]) {
    if (argc < 1) return 0;

    const char *progname = path_basename(argv[0]);

    /* If argv[0] doesn't name an execline command, use argv[1] as subcommand.
     * This covers:
     *   execline define ...        — progname="execline"
     *   ccraft execline.c define ... — progname="execline.c"
     *   /tmp/execline_test define ... — progname="execline_test" */
    if (find_cmd(progname) == NULL) {
        if (argc < 2) return 0;
        return cmd_dispatch(argc - 1, argv + 1);
    }

    return cmd_dispatch(argc, argv);
}
