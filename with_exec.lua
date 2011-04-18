module(..., package.seeall)

require "posix"
require "with_exec_c"

MOUNTPOINT = with_exec_c.MOUNTPOINT
RUNFILE = with_exec_c.RUNFILE
VERSION = with_exec_c.VERSION
ENOENT = with_exec_c.ENOENT
EEXIST = with_exec_c.EEXIST
SIGTERM = with_exec_c.SIGTERM

function quoteNilStr(str)
    return str and string.format("%q", str) or "nil"
end

function quoteStrList(tbl)
    local ret = "{ "
    local strList = {}
    for _, v in ipairs(tbl) do
        table.insert(strList, string.format("%q", v))
    end
    return "{ " .. table.concat(strList, ", ") .. " }"
end

function file_exists(path)
    stat,err,errno = posix.stat(path)
    if stat then
        return true
    elseif errno == ENOENT then
        return false
    else
        error("stat " .. path .. ": " .. err)
    end
end

function show_directory(directory)
    local files = posix.files(directory)
    local ret = {}

    -- Check that we can inspect the contents of the directory
    if not files then
        errstr, errno = posix.errno()
        error("opendir " .. directory .. " failed: " .. errstr)
    end

    -- Iterate over the files.  If the file is a symlink, add the from-to
    -- relationship.  If the file is a directory, then we want to recurse into
    -- that directory and expose the symlinks in that directory.
    for file in files do
        if file ~= '.' and file ~= '..' then
            local fpath = directory .. "/" .. file
            local ftype = posix.stat(fpath)['type']
            if ftype == "directory" then
                table.insert(ret, { from = file,  to = show_directory(fpath) })
            elseif ftype == "link" then
                table.insert(ret, { from = file,  to = posix.readlink(fpath) })
            else
                table.insert(ret, { from = file,  to = nil })
            end
        end
    end

    table.sort(ret, function (a, b) return a.from < b.from; end)
    return ret
end

-- with_exec.execp is a wrapper around execvp(3)
-- Wraps a version incompat:
--   liblua5.1-posix0 1.0 (Hardy) exports posix.exec which calls execvp
--   liblua5.1-posix1 5.1.4 (Lucid) changes posix.exec to call execv, and exports posix.execp instead
execp = posix.execp or posix.exec
__getprocessid = posix.getpid or posix.getprocessid

function getpid()
    return __getprocessid().pid
end

function abspath(path)
    local start = path:sub(1,1)
    if start == '~' then
        error("path " .. path .. " contains unexpanded tilde, read the AcrWith wiki")
    elseif start == "/" then
        return path
    end
    return posix.getcwd() .. "/" .. path
end


-- converts a table to an with_exec command line
-- e.g.:  {a='b', c={d='e'}} ==>  {'--a=b', '--c/d=e'}
function table_to_withexec_argv(t, base_path)
    local result = {}
    local base_path_txt = base_path and base_path .. '/' or ''
    for k, v in pairs(t) do
        if type(v) == 'table' then
            local new_base_path = base_path and string.format('%s%s', base_path_txt, k) or k
            for _, v in ipairs(table_to_withexec_argv(v, new_base_path)) do
                result[#result+1] = v
            end
        else
            result[#result+1] = string.format('%s%s=%s', base_path_txt, k, v)
        end
    end
    return result
end


-- Executes a process in a new namespace.
-- The argument is a table with the following keys:
--
--   cmd:     the command to run. should be a table of strings.
--
--   namespace: a namespace table
--
--   exec_cmd: a table of commands appended to with_exec portion of the command line
--
--   devname: the label which will show up in /proc/pid/mounts. default is with-<pid>.
--
--   dry_run: simply return the lua string to execute, instead of executing.
--
-- If all of targets is empty, then no namespace is created and
-- the program is exec'd directly
function exec(args)
    local namespace, devname, cmd, exec_cmd, dry_run
    for k,v in pairs(args) do
        if k == "namespace" then
            namespace = v
        elseif k == "cmd" then
            cmd = v
        elseif k == "devname" then
            devname = v
        elseif k == "dry_run" then
            dry_run = true
        elseif k == 'exec_cmd' then
            exec_cmd = v
        else
            error("unrecognized argument " .. k)
        end
    end

    if not cmd or #cmd == 0 then
        error("cmd must be non-empty")
    end

    if namespace then
        -- make sure /etc/init.d/with has done its magic.
        if not file_exists(RUNFILE) then
            error("run file " .. RUNFILE ..  " does not exist: mount namespace not inited?")
        end

        -- devname is ignored, but it will show up in /proc/self/mounts
        if not devname then
            devname = "with-" .. getpid()
        end

        local namespace_t = table_to_withexec_argv(namespace)
        if exec_cmd then
            for _, v in ipairs(exec_cmd) do
                namespace_t[#namespace_t + 1] = v
            end
        end

        if dry_run then
            return string.format("with_exec.exec{ devname=%q, targets='%s', cmd='%s' }",
                devname, quoteStrList(namespace_t), quoteStrList(cmd))
        else
            with_exec_c.exec_with_namespace_internal(devname, namespace_t, cmd)
        end
    else
        if dry_run then
            return string.format("with_exec.exec{ cmd = %s }", quoteStrList(cmd))
        else
            -- just execute the command directly
            ignore,err = execp(unpack(cmd))
            error(err)
        end
    end
end

-- Shows the namespace of an existing process
--
-- for from,to in show_namespace(1222) do
--   inside here, from will be the name of an entry in /with,
--   and to will be the value of the symlink inside pid 1222
-- end
--
-- pid can be "self"
function show_namespace(pid)
    local base_directory = "/proc/" .. pid .. "/root/with"
    return show_directory(base_directory)
end

-- exec{ ... cmd = with_exec.shell() } will execute a shell.
function shell()
    return {posix.getenv("SHELL") or "/bin/sh"}
end

-- k5start(cmd) wraps cmd in a k5start invocation that'll read from
-- the users's keytab to obtain kerberos tickets, and optionally AFS tokens.
--
-- Like kexec, this requests renewable tickets good for 10 days.
-- We may need to revisit this policy.
function k5start(cmd, keytab, need_afs)
    if keytab == nil then
        local username = posix.getenv("LOGNAME")
        if not username then
            error("$LOGNAME is nil")
        end
        keytab = "/local/" .. username .. "/private/" .. username .. ".keytab"
    end

    local tbl = {"/usr/bin/k5start", "-q", "-U", "-l", "10d", "-f", keytab}
    if not file_exists(keytab) then
        error("keytab " .. keytab ..  " not found")
    end
    if need_afs then
        table.insert(tbl, "-t")
    end
    table.insert(tbl, "--")
    for _,v in ipairs(cmd) do
        table.insert(tbl, v)
    end
    return tbl
end

-- legacy_setbb(cmd, bb_root) wraps cmd in a bash invocation that'll
-- setbb bb_root before running the command. This should be unneeded
-- when using AcrWith, but in legacy situations it's needed.
function legacy_setbb(cmd, bb_root)
    -- We can't use usebb here because it doesn't exec; that means the pstree
    -- will be caller -> bash -> cmd. Could cause all kinds of problems, since
    -- the bash won't respond to signals correctly or pass on the job's exit
    -- status. Do the exec ourselves so it stays caller -> cmd.
    local tbl = { "/bin/bash", "-c", string.format('source /usr/bin/setbb %s; exec "$0" "$@"', bb_root) }
    for _,v in ipairs(cmd) do
        table.insert(tbl, v)
    end
    return tbl
end

-- Runs a command pipeline, and waits for it to finish.
-- Example:
--   dp = with_exec.daemon_pipe()
--   mypipe = dp:pipe()
--   dp:add_proc{ cmd = {"ls"}, stdout=mypipe }
--   dp:add_proc{ cmd = {"grep", "-v"}, stdin=mypipe }
--   dp:run()
-- is similar to ls | grep -v afs.
--
-- Input/output tokens:
--   dp:pipe(): returns a token which represents a pipe.
--             this token can be passed as stdin/stdout/stderr in add_proc
--
--   file(filename[,append]): returns a token which represents the file
--                            if append is true, the file will be appended to when writing
--
--   dp.devnull: returns a token which represents /dev/null
--
--   dp.caller_stdout:
--   dp.caller_stdin:
--   dp.caller_stderr:
--      returns the stdin/stdout/stderr of the calling process. mostly useful for doing 2>1 style tricks
-- Processes:
--   proc = dp:add_proc{
--      forward_signals = <bool> -- if true, any SIGINT/QUIT/TERM sent to the calling
--                               -- process will be forwarded to this child.
--      cmd = {"cmd","arg1","arg2"} -- Command to run. Will search $PATH
--      stdin/stdout/stderr=<token>
--   }
--   Adds to the list of processes to run and returns a handle to the process.
--   Methods on the handle:
--     proc.finished -- true if the process ran and then finished
--     proc.pid -- the pid, or nil if the process didn't get started
--     proc.WIFEXITED
--     proc.WIFSIGNALED
--     proc.WEXITSTATUS
--     proc.WTERMSIG     -- see documentation in wait(2)
--
--   dp.lock_file: if non-empty, this file will be flock-ed and
--                 the caller's PID written to it
--
--   dp:run(): runs all the processes and waits for them to finish.
--     Returns a table of exit statuses, one for time you called add_proc. The keys
--       {
--         pid = xxx,
--         exited = <true/false>,
--         exitstatus = number, -- defined if exited == true, see WEXITSTATUS
--         signalled = <true/false>,
--         termsig = number,    -- defined if signalled == true, see WTERMSIG
--       }
--
-- The differences from bash's piping:
--   * A process group will be created for all children. This causes ctrl-z to
--     not stop the children (only the caller), and ctrl-c to only be sent to
--     jobs who have forward_signals = true
--
--   * SIGHUP will be set to SIG_IGN for all children.
--
--   * On receipt of a signal, all children must exit before daemon_pipe
--     returns. So no processes should be orphaned unless the parent is kill -9'd
daemon_pipe = with_exec_c.daemon_pipe

-- try_error_write(bbloggercmd, err) is used to exec a bblogger
-- and write the "err" string to it. If this fails, will write
-- err to stderr.
try_error_write = with_exec_c.try_error_write

basename = with_exec_c.basename
dirname = with_exec_c.dirname
