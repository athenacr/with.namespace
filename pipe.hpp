#ifndef WITH_PIPE_H
#define WITH_PIPE_H

#include <stdio.h>
#include <stdexcept>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#include <boost/shared_ptr.hpp>

#include "exec.hpp"

/// RAII class for making sure an file descriptor gets closed
class FD : public boost::noncopyable
{
public:
    static void pipe(FD &readFD, FD &writeFD, int fdflags = 0);

    FD(int fd = -1) : m_fd(fd) {}
    ~FD();

    bool isOk() const { return m_fd != -1; }
    int get() const { if(!isOk()) throw failure("FD::get: invalid fd"); return m_fd; }
    // closes the FD and resets it to newFD
    void reset(int newFD = -1);
    // destructively copies fd from src
    void move_from(FD &src) { reset(src.m_fd); src.m_fd = -1; }

    void setCloseOnExec();
    void setNonBlock();

protected:
    int m_fd;
};
typedef boost::shared_ptr<FD> FDPtr;

/// Installs a sigprocmask to block signals, and restores the
/// mask on exit.
struct SignalBlocker
{
    SignalBlocker();
    ~SignalBlocker();

    /// restores the sigprocmask. our children processes call this; note that this leaves HUP ignored.
    void unblock();

    sigset_t m_sigset, m_oldset;
    struct sigaction m_oldHUPAction;
};

struct file_spec : public boost::noncopyable
{
    file_spec() : m_filename(), m_append(false) {}
    file_spec(std::string const &s, bool append = false)
        : m_filename(s)
        , m_append(append) {}
    std::string m_filename;
    bool m_append;
};
typedef boost::shared_ptr<file_spec> file_spec_ptr;

struct daemon_proc_spec : public boost::noncopyable
{
    daemon_proc_spec()
        : m_forwardSignals(false)
        , m_stdin()
        , m_stdout()
        , m_stderr()
    { resetStatus(); }

    void resetStatus()
    {
        m_pid = -1;
        m_exited = false;
        m_status = 0;
    }

    bool started() const { return m_pid != -1; }
    bool running() const { return m_pid != -1 && !m_exited; }
    bool finished() const { return started() && !running(); }
    int getPID() const { return m_pid; }
    const int &getStatus() const { return m_status; } // byref so WIFEXITED works

    bool m_forwardSignals;
    exec_args m_cmdArgv; // the command we want to run
    file_spec_ptr m_stdin, m_stdout, m_stderr;
    int m_pid;
    bool m_exited;
    int m_status;
};
typedef boost::shared_ptr<daemon_proc_spec> daemon_proc_spec_ptr;

struct daemon_pipe : public boost::noncopyable
{
    struct File
    {
        File(const file_spec_ptr &spec)
            : m_spec(spec)
            , m_append(spec->m_append)
            , m_wantRead(false)
            , m_wantWrite(false) {}
        file_spec_ptr m_spec;
        bool m_append, m_wantRead, m_wantWrite;
        FDPtr m_readSide, m_writeSide;
        void open();
    };

    // serves as a map from file_spec to File, using an unsorted list
    struct FileMap : public boost::noncopyable
    {
        std::vector<File *> m_files;
        ~FileMap() { std::for_each(m_files.begin(), m_files.end(), boost::checked_deleter<File>()); }
        File *get(const file_spec_ptr &spec, bool wantRead, bool wantWrite)
        {
            File *f = NULL;
            for(std::vector<File *>::const_iterator i = m_files.begin(), end = m_files.end();
                    i != end; ++i)
            {
                if((*i)->m_spec == spec)
                {
                    f = *i;
                    break;
                }
            }
            if(!f)
            {
                f = new File(spec);
                m_files.push_back(f);
            }

            f->m_wantRead = f->m_wantRead || wantRead;
            f->m_wantWrite = f->m_wantWrite || wantWrite;
            return f;
        }
    };

    struct Proc
    {
        Proc(daemon_proc_spec_ptr spec)
            : m_spec(spec)
            , m_stdin(NULL)
            , m_stdout(NULL)
            , m_stderr(NULL)
            , m_newPGID(-1)
            , m_blockedSignals(NULL) {}
        int safe_fork_exec();

        daemon_proc_spec_ptr m_spec;
        File *m_stdin, *m_stdout, *m_stderr;
        int m_newPGID;
        SignalBlocker *m_blockedSignals;
    };
    typedef boost::shared_ptr<Proc> ProcPtr;

    file_spec_ptr add_pipe() { return file_spec_ptr(new file_spec); }
    file_spec_ptr add_file(const std::string &filename)
        { return file_spec_ptr(new file_spec(filename)); }
    file_spec_ptr add_file(const std::string &filename, bool append)
        { return file_spec_ptr(new file_spec(filename, append)); }
    file_spec_ptr get_devnull()
        { return get_special(m_devnull, "/dev/null"); }
    file_spec_ptr get_caller_stdin()
        { return get_special(m_caller_stdin, "/dev/stdin"); }
    file_spec_ptr get_caller_stdout()
        { return get_special(m_caller_stdout, "/dev/stdout"); }
    file_spec_ptr get_caller_stderr()
        { return get_special(m_caller_stderr, "/dev/stderr"); }

    void add_proc(const daemon_proc_spec_ptr &spec)
        { m_specs.push_back(spec); }

    std::string m_lockFile;

    void exec();
    void try_error_write(const std::string &input);

private:
    struct LockFile
    {
        ~LockFile();
        void open(const std::string &file);

        FD m_fd;
    };

    file_spec_ptr get_special(file_spec_ptr &member, const char *name)
    {
        if(!member)
            member.reset(new file_spec(name));
        return member;
    }

    std::vector<daemon_proc_spec_ptr> m_specs;
    file_spec_ptr m_devnull, m_caller_stdout, m_caller_stderr, m_caller_stdin;

    friend struct ProcHarvester;
};
typedef boost::shared_ptr<daemon_pipe> daemon_pipe_ptr;

#endif // WITH_PIPE_H
