#include "pipe.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>

#define CHECK(cond, fmt...) \
    do { \
        if(!(cond)) \
            throw failure(fmt); \
    } while(0)

int writeN(int fd, const void *buf, ssize_t count)
{
    const char *bufC = reinterpret_cast<const char *>(buf);
    while(count > 0)
    {
        ssize_t s = ::write(fd, bufC, count);
        if(s <= 0)
            return s;
        bufC += s;
        count -= s;
    }
    return 0;
}

void FD::pipe(FD &readFD, FD &writeFD, int fdflags)
{
    int raw[2];
    CHECK(::pipe(raw) == 0, "pipe failed");
    readFD.reset(raw[0]);
    writeFD.reset(raw[1]);
    if(fdflags == FD_CLOEXEC)
    {
        readFD.setCloseOnExec();
        writeFD.setCloseOnExec();
    }
    else if(fdflags != 0)
        throw failure("FD::pipe: unrecognized fdflags %d", fdflags);
}

FD::~FD()
{
    try {
        reset();
    } catch(...) {}
}

void FD::reset(int newFD)
{
    if(m_fd != -1)
        CHECK(close(m_fd) == 0, "close failed: %m");
    m_fd = newFD;
}

void FD::setCloseOnExec()
{
    int fdflags = fcntl(m_fd, F_GETFD);
    CHECK(fdflags >= 0, "fcntl(F_GETFD) failed: %m");
    CHECK(fcntl(m_fd, F_SETFD, fdflags | FD_CLOEXEC) >= 0,
        "fcntl(F_SETFD) failed: %m");
}

void FD::setNonBlock()
{
    int flags = fcntl(m_fd, F_GETFL);
    CHECK(flags >= 0, "fcntl(F_GETFL) failed: %m");
    CHECK(fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) >= 0,
        "fcntl(F_SETFL) failed: %m");
}

void daemon_pipe::File::open()
{
    if(m_spec->m_filename.empty())
    {
        m_readSide.reset(new FD);
        m_writeSide.reset(new FD);
        FD::pipe(*m_readSide, *m_writeSide, FD_CLOEXEC);
    }
    else
    {
        FDPtr fd(new FD);
        if(m_spec->m_filename == "/dev/stdin")
        {
            CHECK(!m_wantWrite, "caller_stdin cannot be used for writing");
            fd->reset(::dup(STDIN_FILENO));
            CHECK(fd->isOk(), "dup(STDIN_FILENO) failed: %m");
            m_readSide = fd;
        }
        else if(m_spec->m_filename == "/dev/stdout")
        {
            CHECK(!m_wantRead, "caller_stdout cannot be used for reading");
            fd->reset(::dup(STDOUT_FILENO));
            CHECK(fd->isOk(), "dup(STDOUT_FILENO) failed: %m");
            m_writeSide = fd;
        }
        else if(m_spec->m_filename == "/dev/stderr")
        {
            CHECK(!m_wantRead, "caller_stderr cannot be used for reading");
            fd->reset(::dup(STDERR_FILENO));
            CHECK(fd->isOk(), "dup(STDERR_FILENO) failed: %m");
            m_writeSide = fd;
        }
        else
        {
            int mode = 0;
            if(m_wantRead)
            {
                mode |= O_RDONLY;
                m_readSide = fd;
            }
            if(m_wantWrite)
            {
                mode |= O_CREAT | O_WRONLY;
                if(m_append)
                    mode |= O_APPEND;
                m_writeSide = fd;
            }

            fd->reset(::open(m_spec->m_filename.c_str(), mode, 0666));
            CHECK(fd->isOk(), "open %s failed: %m", m_spec->m_filename.c_str());
        }
        fd->setCloseOnExec();
    }
}

// fork+exec, propagates errors in the child back to the parent via a pipe
int daemon_pipe::Proc::safe_fork_exec()
{
    int pid = -1;

    CHECK(!m_spec->m_cmdArgv.empty(), "cmd_argv is empty");
    FD errorPipeRead, errorPipeWrite;
    FD::pipe(errorPipeRead, errorPipeWrite, FD_CLOEXEC);
    errorPipeWrite.setNonBlock();

    pid = fork();
    CHECK(pid >= 0, "fork failed: %m");

    try
    {
        if(pid == 0)
        {
            try
            {
                if(m_newPGID >= 0)
                    CHECK(setpgid(0, m_newPGID) == 0, "setpgid failed: %m");
                if(m_stdin)
                    CHECK(dup2(m_stdin->m_readSide->get(), STDIN_FILENO) >= 0, "dup2 failed: %m");
                if(m_stdout)
                    CHECK(dup2(m_stdout->m_writeSide->get(), STDOUT_FILENO) >= 0, "dup2 failed: %m");
                if(m_stderr)
                    CHECK(dup2(m_stderr->m_writeSide->get(), STDERR_FILENO) >= 0, "dup2 failed: %m");
                if(m_blockedSignals)
                    m_blockedSignals->unblock();

                m_spec->m_cmdArgv.do_execvp();
            }
            catch(failure &e)
            {
                int ret = write(errorPipeWrite.get(), e.what(), strlen(e.what()));
                (void)ret;
            }
            _exit(1);
        }
        errorPipeWrite.reset();
        failure f;
        ssize_t ret = read(errorPipeRead.get(), f.m_err, sizeof(f.m_err));
        CHECK(ret >= 0, "read from error pipe failed: %m");
        if(ret > 0)
            throw f;

        m_spec->m_pid = pid;
        return pid;
    }
    catch(...)
    {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        throw;
    }
}

SignalBlocker::SignalBlocker()
{
    CHECK(sigemptyset(&m_sigset) == 0, "sigemptyset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGCHLD) == 0, "sigaddset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGHUP) == 0, "sigaddset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGTERM) == 0, "sigaddset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGINT) == 0, "sigaddset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGQUIT) == 0, "sigaddset failed: %m");
    CHECK(sigaddset(&m_sigset, SIGPIPE) == 0, "sigaddset failed: %m");

    /// ignore SIGHUP and leave it ignored for our children.
    struct sigaction action = {};
    action.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGHUP, &action, &m_oldHUPAction) == 0, "sigaction failed: %m");

    CHECK(sigprocmask(SIG_BLOCK, &m_sigset, &m_oldset) == 0, "sigprocmask failed: %m");
}

SignalBlocker::~SignalBlocker()
{
    unblock();

    if(sigaction(SIGHUP, &m_oldHUPAction, NULL) == -1 && !std::uncaught_exception())
        throw failure("sigaction failed: %m");
}

/// restores the sigprocmask. our children processes call this; note that this leaves HUP ignored.
void SignalBlocker::unblock()
{
    if(sigprocmask(SIG_SETMASK, &m_oldset, NULL) != 0 && !std::uncaught_exception())
        throw failure("sigprocmask failed: %m");
}


struct ProcHarvester
{
    ProcHarvester(sigset_t *sigset)
        : m_sigset(sigset) {}
    ~ProcHarvester()
    {
        try {
            harvest();
        }
        catch(...) {}
        m_procs.clear();
    }

    daemon_pipe::Proc &addProc(const daemon_proc_spec_ptr &spec)
    {
        spec->resetStatus();
        daemon_pipe::ProcPtr proc(new daemon_pipe::Proc(spec));
        m_procs.push_back(proc);
        return *m_procs.back();
    }

    void harvest()
    {
        while(true)
        {
            bool somethingleft = false;
            // waitpid on each of our children and mark anything that's exited
            std::vector<daemon_pipe::ProcPtr>::iterator i = m_procs.begin(), end = m_procs.end();
            for(; i != end; ++i)
            {
                if(!(*i)->m_spec->running())
                    continue;

                int status;
                int ret = waitpid((*i)->m_spec->m_pid, &status, WNOHANG);
                CHECK(ret >= 0, "waitpid failed: %m");

                if(ret > 0)
                {
                    (*i)->m_spec->m_exited = true;
                    (*i)->m_spec->m_status = status;
                }
                else
                    somethingleft = true;
            }

            if(!somethingleft)
                break;

            int sig;
            CHECK(sigwait(m_sigset, &sig) == 0, "sigwait failed: %m");

            switch(sig)
            {
            // forward these signals onto any of our children that have m_forwardSignals set.
            case SIGTERM:
            case SIGINT:
            case SIGQUIT:
                for(std::vector<daemon_pipe::ProcPtr>::iterator i = m_procs.begin(), end = m_procs.end();
                      i != end; ++i)
                {
                    if((*i)->m_spec->running() && (*i)->m_spec->m_forwardSignals)
                    {
                        CHECK(kill((*i)->m_spec->m_pid, sig) == 0, "kill pid=%d sig=%d failed: %m",
                                (*i)->m_spec->m_pid, sig);
                    }
                }
                break;

            case SIGCHLD: // this'll cause us to reloop and wait for or children

            case SIGHUP:  // we want to just ignore this

            case SIGPIPE: // this could mean bblogger died in try_error_write.
                          // ignore; we'll get EPIPE from ::write() which'll 
                          // cause us to just write to stderr.

            default:
                break;
            }
        }
    }

    std::vector<daemon_pipe::ProcPtr> m_procs;
    const sigset_t *m_sigset;
};

void daemon_pipe::LockFile::open(const std::string &file)
{
    m_fd.reset(::open(file.c_str(), O_CREAT|O_RDWR, 0666));
    CHECK(m_fd.isOk(), "unable to open pidfile %s for writing: %m", file.c_str());
    m_fd.setCloseOnExec();
    int ret = ::flock(m_fd.get(), LOCK_EX | LOCK_NB);
    if(ret != 0)
    {
        m_fd.reset(); // close the FD so we don't truncate in destructor
        if(errno == EWOULDBLOCK)
            throw failure("process is already running (pidfile %s is locked)", file.c_str());
        throw failure("unable to lock pidfile %s", file.c_str());
    }

    CHECK(ftruncate(m_fd.get(), 0) == 0,
            "unable to truncate lockfile %s: %m", file.c_str());
    char buf[256];
    buf[sizeof(buf)-1] = '\0';
    int n = snprintf(buf, sizeof(buf)-1, "%d\n", getpid());
    CHECK(writeN(m_fd.get(), buf, n) == 0,
            "unable to write to lockfile %s: %m", file.c_str());
}

daemon_pipe::LockFile::~LockFile()
{
    if(m_fd.isOk())
    {
        // getting here means we have the file open & locked.
        // since we're shutting down, clear out the pidfile
        // so nothing tries to kill some other process

        // we can't delete the file because someone might have
        // renamed it in the meantime
        int ret = ftruncate(m_fd.get(), 0);
        (void)ret;
        m_fd.reset();
    }
}

void daemon_pipe::exec()
{
    CHECK(!m_specs.empty(), "no procs to execute");

    SignalBlocker signals;

    // LockFile will be unlocked on destruction; don't do this until
    // after the ProcHarvester is destroyed
    LockFile lock;

    // ProcHarvester will wait for all children on destruction. Since we want
    // all the FDs to get closed before that happens, this must be instantiated
    // before the FileMap.
    ProcHarvester harvester(&signals.m_sigset);

    // build a map of all the files we're going to need to open, and whether
    // we need to read or write from them
    FileMap files;
    for(std::vector<daemon_proc_spec_ptr>::iterator i = m_specs.begin(), end = m_specs.end(); i != end; ++i)
    {
        Proc &proc(harvester.addProc(*i));

        if((*i)->m_stdin)
            proc.m_stdin = files.get((*i)->m_stdin, true, false);
        if((*i)->m_stdout)
            proc.m_stdout = files.get((*i)->m_stdout, false, true);
        if((*i)->m_stderr)
            proc.m_stderr = files.get((*i)->m_stderr, false, true);
    }

    if(!m_lockFile.empty())
        lock.open(m_lockFile);

    std::vector<File *>::const_iterator file = files.m_files.begin(),
                                        end = files.m_files.end();
    for(; file != end; ++file)
        (*file)->open();

    int pgid = 0;
    for(std::vector<ProcPtr>::iterator i = harvester.m_procs.begin(), end = harvester.m_procs.end(); i != end; ++i)
    {
        Proc &proc = **i;
        proc.m_blockedSignals = &signals;
        proc.m_newPGID = pgid;
        int pid = proc.safe_fork_exec();
        if(pgid == 0)
            pgid = pid;
    }
}

void daemon_pipe::try_error_write(const std::string &input)
{
    // keep signals blocked even inside our catch, so we can't
    // get SIGPIPE from the fwrite
    SignalBlocker signals;

    try
    {
        CHECK(m_specs.size() == 1, "specs must have 1 element");
        const daemon_proc_spec_ptr &procSpec = m_specs[0];

        {
            ProcHarvester harvester(&signals.m_sigset);

            file_spec_ptr pipe_spec(new file_spec);
            File file(pipe_spec);
            file.open();
            file.m_writeSide->setNonBlock();

            Proc &proc(harvester.addProc(procSpec));

            proc.m_stdin = &file;
            proc.m_blockedSignals = &signals;
            proc.m_newPGID = 0;
            proc.safe_fork_exec();
            file.m_readSide->reset();

            int ret = ::write(file.m_writeSide->get(), input.c_str(), input.length());
            CHECK(ret >= 0, "write failed: %m");

            file.m_writeSide->reset();
        }
        if(!(procSpec->finished() &&
                WIFEXITED(procSpec->getStatus()) &&
                WEXITSTATUS(procSpec->getStatus()) == 0))
            throw failure("proc failed");
    }
    catch(failure &f)
    {
        writeN(STDERR_FILENO, input.c_str(), input.length());
    }
}
