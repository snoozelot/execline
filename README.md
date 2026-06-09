execline — 46 commands, one binary
====================================

execline is a chain-loading command language. Each command modifies
process state (env, fds, cwd, signals) and hands control to the next
command in argv.  No `;`, no `&&`, no `|` — argv is the script.
Each command consumes its args and the dispatch loop passes the
remainder to the next handler.

This is not a shell.  No parser, no glob expansion, no word splitting.
Commands receive argv already split by the caller.

```
  ./execline.c                                    \
      define    MSG hello                         \
      /bin/echo '$MSG'
```

`define` takes 2 args (MSG, hello), substitutes `$MSG` in the
remaining argv.  `echo` receives `hello`.

```
  ./execline.c                                    \
      export    PATH /usr/local/bin:/usr/bin:/bin \
      foreground { echo building }                \
      make all
```

`export` sets PATH.  `foreground` runs the block and waits.
`make all` runs.  State persists — no exec between steps.

```
  ./execline.c                                    \
      backtick  OUTPUT { /bin/echo data }         \
      importas  VAL OUTPUT                        \
      /bin/echo '$VAL'
```

`backtick` captures output into `$OUTPUT`.  `importas` reads it
back.  `echo` prints the captured value.

46 commands
-----------

### Environment

```
  ./execline.c                                    \
      export    FOO hello                         \
      importas  VAL FOO                           \
      /bin/echo '$VAL'
```

**export** — setenv(var, value).  `importas` reads FOO from the
environment, substitutes `$VAL`.  **unexport** — unsetenv(var).
**empty** — unset named vars; `-P` clears `#`, `0`-`9`, `?`, `!`.
**emptyenv** — clear entire env; `-P` preserves special vars.
**envfile** — load `KEY=val` lines from a file (default: `./env`).

### Substitution

```
  ./execline.c                                    \
      define    FOO hello                         \
      /bin/echo '$FOO world'
```

**define** — replace `$KEY` or `${KEY}` with VALUE in remaining argv.
`-s` splits value on delimiter (one word per element).  `-C` crunches
consecutive delimiters.  `-d delim` sets delimiter.  Backslash quoting:
odd count before `$KEY` = literal; even count = substitute, keep half.

```
  ./execline.c                                    \
      importas  VAL FOO                           \
      /bin/echo '$VAL'
```

**importas** — read env var FOO, substitute `$VAL` with its value.
`-D default` when unset.  `-i` insists (errors if unset).  `-u` unsets
the source var after reading.

```
  ./execline.c                                    \
      multidefine A prefixA B prefixB --          \
      /bin/echo '$A/$B'
```

**multidefine** — define multiple key/value pairs (end at `--`),
substitute simultaneously.

```
  ./execline.c                                    \
      multisubstitute                             \
          define    A x                           \
          define    B y                           \
      ''                                          \
      /bin/echo '$A $B'
```

**multisubstitute** — collect define/importas/elglob in a block
(terminated by `''`), apply all substitutions at once to the prog.

### Conditional execution

```
  ./execline.c                                    \
      if        /bin/test -d /tmp                 \
      ''                                          \
      /bin/echo '/tmp exists'
```

**if** — run a block.  If it exits 0, run the chain.  `-n` inverts.
`-t CODE` / `-x CODE` match specific exit codes.  Sets `?`.

```
  ./execline.c                                    \
      ifelse    /bin/test -d /tmp                 \
      ''                                          \
      /bin/echo '/tmp exists'                     \
      ''                                          \
      /bin/echo '/tmp missing'
```

**ifelse** — three blocks: condition, then, else.

```
  ./execline.c                                    \
      ifthenelse                                  \
          /bin/test -f /tmp/foo                   \
      ''                                          \
          /bin/echo 'exists'                      \
      ''                                          \
          /bin/echo 'missing'                     \
      ''                                          \
      /bin/echo 'checked'
```

**ifthenelse** — three blocks (condition, then, else) and a rest
chain that runs regardless.

```
  ./execline.c                                    \
      foreground { /bin/echo hello }              \
      /bin/echo world
```

**foreground** — run a block in a child, wait, set `?` to exit code,
continue.

```
  ./execline.c                                    \
      background { sleep 5 }                      \
      /bin/echo 'immediate'
```

**background** — fork a block, continue immediately.  Sets `!` to
child PID.

```
  ./execline.c                                    \
      pipeline { /bin/echo data }                 \
      tr '[:lower:]' '[:upper:]'
```

**pipeline** — run block with stdout connected to next command's
stdin.

```
  ./execline.c                                    \
      fdclose   7                                 \
      fdclose   8                                 \
      piperw    7 8                               \
      /bin/echo ok
```

**piperw** — create a pipe on two specified file descriptors.

### I/O redirection

```
  ./execline.c                                    \
      redirfd   -w 1 /tmp/out.txt                 \
      /bin/echo hello
```

**redirfd** — open a file and dup2 to target fd.
`-r` (read), `-w` (write/trunc), `-a` (append), `-u` (rw),
`-x` (excl create).  `-n` sets O_NONBLOCK.  `-b` toggles blocking.

```
  ./execline.c                                    \
      fdmove    1 2                               \
      /bin/echo ok
```

**fdmove** — dup2(old, new).  `-c` closes old fd after move.

```
  ./execline.c                                    \
      fdclose   9                                 \
      /bin/echo ok
```

**fdclose** — close(fd).

```
  ./execline.c                                    \
      fdreserve 7                                 \
      /bin/echo ok
```

**fdreserve** — ensure fd closed, open /dev/null on it.

```
  ./execline.c                                    \
      fdreserve 7  fdreserve 8                    \
      fdswap    7  8                              \
      /bin/echo ok
```

**fdswap** — swap two fds via intermediate dup.

```
  ./execline.c                                    \
      fdblock   0                                 \
      /bin/echo ok
```

**fdblock** — clear O_NONBLOCK on a fd.

```
  ./execline.c                                    \
      heredoc   hello                             \
      ''                                          \
      tr '[:lower:]' '[:upper:]'
```

**heredoc** — write block content (joined by newlines) to temp file,
feed as stdin.

```
  ./execline.c                                    \
      withstdinas /tmp/data.txt                   \
      tr '[:lower:]' '[:upper:]'
```

**withstdinas** — open file, dup2 to fd 0 (stdin).

### Data capture

```
  ./execline.c                                    \
      backtick  OUTPUT { /bin/echo data }         \
      importas  VAL OUTPUT                        \
      /bin/echo '$VAL'
```

**backtick** — run block, capture stdout, strip trailing newline,
store in env var.  `-0` for null-delimited.  `-D default` on failure.
`-i` / `-I` / `-x` control behavior on subprocess exit.

```
  ./execline.c                                    \
      forbacktickx VAR { /bin/echo -e 'a\\nb' }   \
      ''                                          \
      importas  VAL VAR                           \
      /bin/echo '$VAL'
```

**forbacktickx** — run generator, split output (newline or `-0`),
iterate.  Each value sets VAR, runs loop body.  `-o` / `-x` filter
on exit codes.

```
  ./execline.c                                    \
      forx      VAR a b                           \
      ''                                          \
      importas  VAL VAR                           \
      /bin/echo '$VAL'
```

**forx** — iterate over literal values from a block.
Each value sets VAR, runs loop body.

```
  /bin/echo -e 'a\\nb'                            \
      | ./execline.c                              \
          forstdin  VAR                           \
          importas  VAL VAR                       \
          /bin/echo '$VAL'
```

**forstdin** — read stdin line by line (or `-0` records), iterate.
Each line sets VAR, runs loop body.  `-w` strips whitespace.
`-W` squeezes internal whitespace.

### Process management

```
  ./execline.c                                    \
      exec      /bin/echo hello world
```

**exec** — replace the process.  `-c` clears env.  `-l` prepends `-`
to argv[0] (login shell).  `-a argv0` sets argv[0].

```
  ./execline.c                                    \
      tryexec   /nonexistent                      \
      /bin/echo fallback
```

**tryexec** — attempt exec; on failure, chain continues with the
remaining argv (skipping the failed program).

```
  ./execline.c                                    \
      background { sleep 1 }                      \
      wait
```

**wait** — wait for children.  Accepts a block of PIDs.  `-t timeout`
uses SIGALRM.  `-I` silently handles no-children errors.

```
  ./execline.c                                    \
      trap                                        \
          USR1  { /bin/echo caught }              \
      ''                                          \
      getpid    PP                                \
      foreground importas VAL PP kill -USR1 '$VAL'\
      ''
```

**trap** — install signal handlers in-process.  Syntax: `trap {
SIGNAME { action... } ... } prog...`.  Signals that arrive during
the chain are dispatched after it completes.  Each action runs in
a forked child.

```
  ./execline.c                                    \
      exit      42
```

**exit** — exit with given code (default 0).

### Information

```
  ./execline.c                                    \
      getpid    MYPID                             \
      importas  VAL MYPID                         \
      /bin/echo '$$ = $VAL'
```

**getpid** — store PID into an env var.  `-P` prints it.  `-e` / `-E`
controls export.

```
  ./execline.c                                    \
      getcwd    MYPATH                            \
      importas  VAL MYPATH                        \
      /bin/echo 'cwd = $VAL'
```

**getcwd** — store current working directory into env var.

**dollarat** — read positional params (`#`, `1`, `2`, ...) from env,
print space-separated.  `-d delim` sets delimiter.  `-n` suppresses
trailing newline.  (run with `env '#=2' '1=a' '2=b'`)

```
  ./execline.c                                    \
      eltest    -d /tmp
```

**eltest** — inline POSIX test(1).  No fork.  Supports `-d`, `-e`,
`-f`, `-n`, `-z`, string `=` / `!=`, numeric `-eq` / `-ne` / `-gt`
/ `-ge` / `-lt` / `-le`, file age (`-nt`, `-ot`, `-ef`),
`-a` / `-o`, `( )` grouping.

```
  ./execline.c                                    \
      elglob    FILES '*.c'                       \
      importas  VAL FILES                         \
      /bin/echo 'files: $VAL'
```

**elglob** — glob a pattern, store space-separated results in env
var.  `-v` prints.  `-w` uses pattern as-is on no match.
`-m` allows literal pattern fallback.

```
  ./execline.c                                    \
      case      hello                             \
          '*ell*' { /bin/echo matched }           \
      ''                                          \
      /bin/echo nomatch
```

**case** — fnmatch value against patterns in a block.  First match
runs its action.  `-i` case-insensitive (needs GNU libc).  `-n` inverts.

### Directory and mask

```
  ./execline.c                                    \
      cd        /tmp                              \
      getcwd    PWD                               \
      importas  VAL PWD                           \
      /bin/echo '$VAL'
```

**cd** — chdir + update PWD.  **posix-cd** — chdir only, no PWD
update.  **umask** — set file creation mask (octal).  **posix-umask**
— same, no frills.

### Scripting

```
  ./execline.c                                    \
      execlineb -c 'echo hello world'
```

**execlineb** — parse execline script text with `{ }` blocks, `""`
strings, `#` comments, `$@` expansion.  Accepts `-c script` or a
script file path + args (positional params).

```
  ./execline.c                                    \
      runblock  1                                 \
          /bin/echo hello                         \
      ''                                          \
      world
```

**runblock** — run a block with up to N positional args from prog
appended.  The rest of prog (beyond N) is the chain.

```
  ./execline.c                                    \
      elgetpositionals MYVAR 1                    \
      /bin/echo '$MYVAR'
```

**elgetpositionals** — collect env `$N..$M` (with shift) into a var,
substitute `$KEY` in remaining argv.  `-D default` when shift exceeds
args.  `-i` insists.  `-s` splits.  (run with `env '#=3' '1=a'
'2=b' '3=c'`)

Usage
-----

```
  ./execline.c export FOO hello /bin/echo '$FOO'
  ccraft -o execline execline.c
  ./execline export FOO hello /bin/echo '$FOO'
  ln -s execline export
  ./export FOO hello /bin/echo '$FOO'
```

Compile dep: [ccraft](https://github\.com/snoozelot/ccraft).
Standalone: `cc -std=gnu99 -D_GNU_SOURCE execline.c -o execline`.

Design
------

46 commands, one file.  Each modifies state and returns.  Dispatch
picks the next from argv, calls its handler.  No exec between commands
— state accumulates through the chain.

Substitution engine replaces `$KEY` / `${KEY}` in argv.  Backslash
quoting, split values, simultaneous multi-key substitution.
Block syntax: flat argv convention — args space-prefixed per nesting
depth, terminated by empty strings.

Testing
-------

```
  ./execline.t              all 147 tests
  ./execline.t /backtick    filter
  ./execline.t -f           3/3 mutations detected
  ./execline.t -l           list
```

References
----------

- [execline](https://skarnet.org/software/execline/) — command
  semantics reference
- [ccraft](https://github\.com/snoozelot/ccraft) — compile-and-run
  for C
