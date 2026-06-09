execline —  Chain-loading command language
==========================================

execline is a chain-loading command language. Each command modifies
process state (env, fds, cwd, signals) and hands control to the next
command in argv.  No `;`, no `&&`, no `|` — argv is the script.
Each command consumes its args and the dispatch loop passes the
remainder to the next handler.

This is not a shell.  No parser, no glob expansion, no word splitting.
Commands receive argv already split by the caller.

Pipe echo output through a reader loop:

```
  ./execline.c                             \
      pipeline  /bin/echo 'hello execline' \
      ''                                   \
      forstdin  LINE                       \
          importas  MSG LINE               \
          /bin/echo 'got: $MSG'
```

`pipeline` forks the block (`echo`) and connects its stdout to
the next command's stdin.  `forstdin` reads each line, sets the
`LINE` env var.  `importas` brings `LINE` into argv substitution
as `$MSG`.  Every step runs in the same process — no exec between
execline commands.

Block arguments (like `echo hello execline` above) are terminated
by an empty string `''` — that is how blocks work in flat argv.
`execlineb` scripts use `{ }` syntax instead; see execlineb(1).

Quick reference
---------------

## Environment

| Command | What it does |
|---------|--------------|
| [export](#export) | setenv(key, val) |
| [unexport](#unexport) | unsetenv(key) |
| [empty](#empty) | unset named vars; `-P` clears special vars |
| [emptyenv](#emptyenv) | clear entire env; `-P` preserves special vars |
| [envfile](#envfile) | load `KEY=val` from a file (default `./env`) |

## Substitution

| Command | What it does |
|---------|--------------|
| [define](#define) | replace `$KEY` / `${KEY}` with value in remaining argv |
| [importas](#importas) | read env var, substitute `$VAR` |
| [multidefine](#multidefine) | define multiple key/value pairs, substitute simultaneously |
| [multisubstitute](#multisubstitute) | collect define/importas/elglob in a block, apply all at once |

## Conditional execution

| Command | What it does |
|---------|--------------|
| [if](#if) | run block; exits 0 → run chain |
| [ifelse](#ifelse) | three blocks: condition, then, else |
| [ifthenelse](#ifthenelse) | three blocks + rest chain |
| [foreground](#foreground) | run block in child, wait |
| [background](#background) | fork block, continue immediately |
| [pipeline](#pipeline) | run block with stdout → next stdin |
| [piperw](#piperw) | create a pipe on two fds |

## I/O redirection

| Command | What it does |
|---------|--------------|
| [redirfd](#redirfd) | open file, dup2 to target fd |
| [fdmove](#fdmove) | dup2(old, new) |
| [fdclose](#fdclose) | close(fd) |
| [fdreserve](#fdreserve) | ensure fd closed, open /dev/null on it |
| [fdswap](#fdswap) | swap two fds via intermediate dup |
| [fdblock](#fdblock) | clear O_NONBLOCK on fd |
| [heredoc](#heredoc) | write block to temp file, feed as stdin |
| [withstdinas](#withstdinas) | open file, dup2 to fd 0 |

## Data capture

| Command | What it does |
|---------|--------------|
| [backtick](#backtick) | run block, capture stdout, store in env var |
| [forbacktickx](#forbacktickx) | run generator, split output, iterate |
| [forx](#forx) | iterate over literal values from a block |
| [forstdin](#forstdin) | read stdin line by line, iterate |

## Process management

| Command | What it does |
|---------|--------------|
| [exec](#exec) | replace the process |
| [tryexec](#tryexec) | attempt exec; on failure chain continues |
| [wait](#wait) | wait for children |
| [trap](#trap) | install signal handlers in-process |
| [exit](#exit) | exit with given code (default 0) |

## Information

| Command | What it does |
|---------|--------------|
| [getpid](#getpid) | store PID in env var |
| [getcwd](#getcwd) | store cwd in env var |
| [dollarat](#dollarat) | print positional params |
| [eltest](#eltest) | inline POSIX test(1), no fork |
| [elglob](#elglob) | glob pattern, store results in env var |
| [case](#case) | fnmatch value against patterns |

## Directory and mask

| Command | What it does |
|---------|--------------|
| [cd](#cd) | chdir + update PWD |
| [posix-cd](#posix-cd) | chdir only, no PWD update |
| [umask](#umask) | set file creation mask (octal) |
| [posix-umask](#posix-umask) | same, no frills |

## Scripting

| Command | What it does |
|---------|--------------|
| [execlineb](#execlineb) | parse execline script text |
| [runblock](#runblock) | run block with up to N positional args |
| [elgetpositionals](#elgetpositionals) | collect env `$N..$M` into var |

Commands
--------

## export

`export [ -D default ] [ -i ] key val prog...`

Set environment variable `key` to `val`.  If `key` is already set,
it is overwritten.

- `-D default` — set a fallback value if `key` is unset
- `-i` — error if `key` is not found

**Example:** `export FOO hello /bin/echo '$FOO'`

## unexport

`unexport key prog...`

Unset environment variable `key`.

## empty

`empty [ -P ] var... prog...`

Unset one or more named environment variables.

- `-P` — also clear special internal vars `#`, `0`-`9`, `?`, `!`

## emptyenv

`emptyenv [ -P ] prog...`

Clear the entire environment.

- `-P` — preserve `PATH` and the internal state vars `#`,
  `0`-`9`, `?`, `!`

## envfile

`envfile [ -i | -I ] [ file ] prog...`

Read a file of `KEY=value` assignments (default `./env`) and export
each pair into the environment.  Supports bash-style comments (`#`),
line continuations (`\`), and quoted values (`"` and `'`).

- `-i` — error if file is missing (default)
- `-I` — silently skip if file is missing
- `-` as file reads from stdin

## define

`define [ -s [ -C ] [ -d delim ] ] key val prog...`

Replace `$KEY` or `${KEY}` with `val` in the remaining argv.

- `-s` — split `val` on delimiter, producing one word per element
- `-C` — crunch consecutive delimiters (implies `-s`)
- `-d delim` — set the split delimiter (default: whitespace)

Backslash quoting: an odd number of backslashes before `$KEY` makes
it literal; an even number substitutes and keeps half the backslashes.

**Example:** `define MSG hello /bin/echo '$MSG'`

## importas

`importas [ -D default ] [ -i ] [ -u ] var key prog...`

Read environment variable `key` and substitute `$var` with its value
in the remaining argv.

- `-D default` — use this value if `key` is unset
- `-i` — insist: error if `key` is not found
- `-u` — unset `key` from the environment after reading

## multidefine

`multidefine key val key2 val2 ... -- prog...`

Define multiple key/value pairs, ending at `--`.  All substitutions
are applied simultaneously to the remaining argv.

## multisubstitute

`multisubstitute { define ... | importas ... | elglob ... } '' prog...`

Collect define/importas/elglob commands inside a block (terminated
by `''`).  All substitutions are applied simultaneously to the prog.

## if

`if [ -n ] [ -t code ] [ -x code ] { block } '' prog...`

Run `block`.  If it exits 0, execute the chain.  Sets `?` to the
exit code.

- `-n` — invert: run chain if block exits non-zero
- `-t code` — match specific exit code
- `-x code` — match specific exit code (inverted sense)

**Example:** `if /bin/test -d /tmp '' /bin/echo '/tmp exists'`

## ifelse

`ifelse { condition } '' { then-block } '' { else-block } '' prog...`

Three blocks: condition, then, else.  If condition exits 0, run
then-block; otherwise run else-block.  Then run prog.

## ifthenelse

`ifthenelse { condition } '' { then-block } '' { else-block } '' prog...`

Same as ifelse, but prog runs regardless of which branch executed.

## foreground

`foreground { block } prog...`

Run `block` in a child process and wait for it to finish.  Sets `?`
to the child's exit code.  Then run prog.

**Example:** `foreground { /bin/echo hello } /bin/echo world`

## background

`background { block } prog...`

Fork `block` and run it in the background.  Continue immediately
with prog.  Sets `!` to the child PID.

**Example:** `background { sleep 5 } /bin/echo immediate`

## pipeline

`pipeline { block } prog...`

Run `block` with its stdout connected to prog's stdin.

**Example:** `pipeline { /bin/echo data } tr '[:lower:]' '[:upper:]'`

## piperw

`piperw r w prog...`

Create a pipe.  `r` is set to the read end, `w` to the write end.
The pipe is kept open while prog runs.

**Example:** `piperw 7 8 /bin/echo ok`

## redirfd

`redirfd [ -r | -w | -a | -u | -x ] [ -n ] [ -b ] fd file prog...`

Open `file` and dup2 it to target `fd`.  Then run prog.

- `-r` — open for reading
- `-w` — open for writing (truncate)
- `-a` — open for appending
- `-u` — open for reading and writing
- `-x` — open for writing, exclusive create (fail if exists)
- `-n` — set O_NONBLOCK
- `-b` — toggle blocking (clear O_NONBLOCK)

**Example:** `redirfd -w 1 /tmp/out.txt /bin/echo hello`

## fdmove

`fdmove [ -c ] new old prog...`

dup2(old, new).  After the move, both fds refer to the same file
description.

- `-c` — close old fd after the move

**Example:** `fdmove 1 2 /bin/echo ok` — redirect stdout to stderr

## fdclose

`fdclose fd prog...`

Close `fd`.

## fdreserve

`fdreserve fd prog...`

Ensure `fd` is closed, then open /dev/null on it (preventing other
code from using that fd number).

## fdswap

`fdswap a b prog...`

Swap two fds using an intermediate dup.  After the swap, `a` refers
to what `b` did and vice versa.

**Example (chained):** `fdreserve 7 fdreserve 8 fdswap 7 8 /bin/echo ok`
reserves fds 7 and 8, then swaps them.

## fdblock

`fdblock fd prog...`

Clear O_NONBLOCK on `fd` (make it blocking).

## heredoc

`heredoc [ -r | -w ] { content... } '' prog...`

Write block content to a temporary file, then feed that file as stdin
to prog.  Block args (terminated by `''`) are joined with newlines.
The file is deleted after the child reads it.

- `-r` — feed `/dev/fd/N` instead of a dup'd fd
- `-w` — open for writing (default: read)

**Example:** `heredoc hello '' tr '[:lower:]' '[:upper:]'`

## withstdinas

`withstdinas file prog...`

Open `file` for reading and dup2 to fd 0 (stdin).

## backtick

`backtick [ -i | -I | -x | -D default ] [ -N | -n ] [ -E | -e ] [ -0 ] var { block } prog...`

Run `block`, capture its stdout, strip trailing newline, store the
result in environment variable `var`.  Then run prog.

- `-i` — insist on success: error if block fails (default)
- `-I` — ignore exit status: always use captured output
- `-x` — accept any exit status, set `?` env var
- `-D default` — use this value when block fails (implies `-x`)
- `-N` — don't strip trailing newline
- `-n` — strip trailing newline (default, no-op flag)
- `-E` — export var to environment (default)
- `-e` — don't export var
- `-0` — null-delimited output

**Example:** `backtick OUTPUT { /bin/echo data } importas VAL OUTPUT /bin/echo '$VAL'`

## forbacktickx

`forbacktickx [ -0 ] [ -o code ] [ -x code ] var { generator } '' loop-body...`

Run `generator`, split its output on newlines (or nulls with `-0`).
For each value, set `var` and run the loop body.

- `-0` — null-delimited records
- `-o code` — only iterate if generator exits with code
- `-x code` — skip iteration if generator exits with code

## forx

`forx var { values... } '' loop-body...`

Iterate over literal values from a block.  Each value sets `var`,
then runs the loop body.

**Example:** `forx VAR a b '' importas VAL VAR /bin/echo '$VAL'`

## forstdin

`forstdin [ -0 ] [ -w ] [ -W ] var loop-body...`

Read stdin line by line (or null-delimited records with `-0`).
For each line, set `var` and run the loop body.

- `-w` — strip leading and trailing whitespace from each line
- `-W` — squeeze internal whitespace

## exec

`exec [ -c ] [ -l ] [ -a argv0 ] program args...`

Replace the current process with `program`.  Does not return.

- `-c` — clear the environment before exec
- `-l` — prepend `-` to argv[0] (login shell convention)
- `-a argv0` — set argv[0] explicitly

## tryexec

`tryexec program args...`

Attempt to exec `program`.  If exec fails (program not found, no
permission, etc.), the chain continues with the remaining argv —
skipping the failed program and its args.

## wait

`wait [ -t timeout ] [ -I ] { pids... } '' prog...`

Wait for child processes.

- `-t timeout` — use SIGALRM to time out after `timeout` seconds
- `-I` — silently handle the case of no children (don't error)

Accepts a block of PIDs.  If omitted, waits for all children.

## trap

`trap { name { action... } ... } '' prog...`

Install signal handlers in-process.  Syntax: a block containing
signal name / action pairs, terminated by `''`.  Signals that arrive
during prog are dispatched after prog completes.  Each action runs
in a forked child.

**Example:**
```
trap USR1 { /bin/echo caught } '' \
  getpid PP foreground importas VAL PP kill -USR1 '$VAL' ''
```

## exit

`exit [ code ]`

Exit with `code` (default 0).

## getpid

`getpid [ -P ] [ -e | -E ] var prog...`

Store the current process PID into environment variable `var`.

- `-P` — print PID to stdout instead of storing
- `-e` — export `var` to the environment (default)
- `-E` — do not export `var` to the environment

**Example:** `getpid MYPID importas VAL MYPID /bin/echo '$$ = $VAL'`

## getcwd

`getcwd var prog...`

Store the current working directory into environment variable `var`.

**Example:** `getcwd MYPATH importas VAL MYPATH /bin/echo 'cwd = $VAL'`

## dollarat

`dollarat [ -d delim ] [ -n ] prog...`

Read positional parameters (`#`, `1`, `2`, ...) from the environment
and print them space-separated.

- `-d delim` — use `delim` instead of space as separator
- `-n` — suppress trailing newline

**Example:** `env '#=2' '1=a' '2=b' ./dollarat` prints `a b`

## eltest

`eltest [ -d | -e | -f | -n | -z | cond ] prog...`

Inline POSIX test(1) — no fork.  Supports:

- File tests: `-d file`, `-e file`, `-f file`
- String tests: `-n str`, `-z str`, `s1 = s2`, `s1 != s2`
- Numeric tests: `n1 -eq n2`, `-ne`, `-gt`, `-ge`, `-lt`, `-le`
- File age: `f1 -nt f2`, `f1 -ot f2`, `f1 -ef f2`
- Connectives: `-a` (and), `-o` (or), `( )` grouping

Sets `?` to 0 if true, 1 if false.  Runs prog only if true.

**Example:** `eltest -d /tmp /bin/echo '/tmp exists'`

## elglob

`elglob [ -v ] [ -w ] [ -m ] var pattern prog...`

Glob `pattern` and store the space-separated results in environment
variable `var`.

- `-v` — print results to stdout as well
- `-w` — use pattern literally when no match (no error)
- `-m` — allow literal pattern as fallback when no match

**Example:** `elglob FILES '*.c' importas VAL FILES /bin/echo 'files: $VAL'`

## case

`case [ -s | -S ] [ -e | -E ] [ -i ] [ -n | -N ] value { pattern { action... } ... } prog...`

Match `value` against patterns inside a block.  The first matching
pattern runs its action.  If no match, runs prog.

- `-s` — shell glob matching (fnmatch, default)
- `-S` — extended regex matching
- `-e` — basic regex (implies `-S`)
- `-E` — extended regex (default when `-S`)
- `-i` — case-insensitive matching
- `-n` — invert: run action on non-match
- `-N` — enable subexpression capture into `$0`, `$1`, ... (implies `-S`)

**Example:** `case hello '*ell*' { /bin/echo matched } '' /bin/echo nomatch`

## cd

`cd dir prog...`

Change directory to `dir` (chdir) and update the `PWD` environment
variable.

## posix-cd

`posix-cd dir prog...`

Change directory to `dir` (chdir).  Does not update `PWD`.

## umask

`umask mask prog...`

Set the file creation mask to `mask` (octal).

## posix-umask

`posix-umask mask prog...`

Identical to umask.  Provided for compatibility with skarnet
execline scripts that call `posix-umask`.

## execlineb

`execlineb [ -c script | file ] args...`

Parse execline script text.  Supports `{ }` blocks, `""` strings,
`#` comments, and `$@` expansion.  Accepts either `-c script` or a
script file path followed by positional arguments.

**Example:** `execlineb -c 'echo hello world'`

## runblock

`runblock [ n ] { block } '' prog...`

Run `block` with up to `n` positional arguments from prog appended.
The rest of prog (beyond `n` args) is the chain that runs after
the block completes.

**Example:** `runblock 1 /bin/echo hello '' world`

## elgetpositionals

`elgetpositionals [ -D default ] [ -i ] [ -s ] var shift prog...`

Collect positional parameters from environment `$N..$M` (with shift)
into a variable.  Substitutes `$var` in the remaining argv.

- `-D default` — use this value when shift exceeds available args
- `-i` — insist: error if shift exceeds available args
- `-s` — split the value on whitespace

**Example:** `env '#=3' '1=a' '2=b' '3=c' ./elgetpositionals MYVAR 1`

Usage
-----

```
  ./execline.c export FOO hello /bin/echo '$FOO'
  ccraft -o execline execline.c
  ./execline export FOO hello /bin/echo '$FOO'
  ln -s execline export
  ./export FOO hello /bin/echo '$FOO'
```

Compile dep: [ccraft](https://github.com/snoozelot/ccraft).
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
- [ccraft](https://github.com/snoozelot/ccraft) — compile-and-run
  for C
