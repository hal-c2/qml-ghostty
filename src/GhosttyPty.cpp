#include "GhosttyPty.h"

#include <QSocketNotifier>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

GhosttyPty::GhosttyPty(QObject* parent) : QObject(parent) {}

GhosttyPty::~GhosttyPty() {
#if defined(Q_OS_UNIX)
    if (m_fd >= 0) ::close(m_fd);
    if (m_pid <= 0) return;
    // Hang up like a closed terminal window, then make sure nothing lingers.
    const pid_t pid = static_cast<pid_t>(m_pid);
    kill(pid, SIGHUP);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (waitpid(pid, nullptr, WNOHANG) != 0) return;
        usleep(5000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
#endif
}

void GhosttyPty::setProgram(const QString& program) {
    if (m_program == program) return;
    m_program = program;
    Q_EMIT programChanged();
}

void GhosttyPty::setArguments(const QStringList& arguments) {
    if (m_arguments == arguments) return;
    m_arguments = arguments;
    Q_EMIT argumentsChanged();
}

void GhosttyPty::setWorkingDirectory(const QString& directory) {
    if (m_workingDirectory == directory) return;
    m_workingDirectory = directory;
    Q_EMIT workingDirectoryChanged();
}

void GhosttyPty::setEnvironment(const QStringList& environment) {
    if (m_environment == environment) return;
    m_environment = environment;
    Q_EMIT environmentChanged();
}

#if defined(Q_OS_UNIX)

bool GhosttyPty::start(int cols, int rows) {
    if (running()) return true;

    QByteArray program = m_program.toLocal8Bit();
    if (program.isEmpty()) program = qgetenv("SHELL");
    if (program.isEmpty()) program = "/bin/sh";

    // Everything the child needs is built before fork: only async-signal-safe
    // calls are allowed between fork and exec.
    QList<QByteArray> argStorage{program};
    for (const QString& argument : m_arguments) argStorage.append(argument.toLocal8Bit());
    std::vector<char*> argv;
    for (QByteArray& argument : argStorage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    QList<QByteArray> envStorage{"TERM=xterm-256color", "COLORTERM=truecolor"};
    for (const QString& entry : m_environment) envStorage.append(entry.toLocal8Bit());
    const QByteArray directory = m_workingDirectory.toLocal8Bit();

    struct winsize size = {};
    size.ws_col = static_cast<unsigned short>(qBound(1, cols, 65535));
    size.ws_row = static_cast<unsigned short>(qBound(1, rows, 65535));

    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, &size);
    if (pid < 0) {
        Q_EMIT errorOccurred(QStringLiteral("forkpty failed: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    if (pid == 0) {
        for (QByteArray& entry : envStorage) putenv(entry.data());
        if (!directory.isEmpty() && chdir(directory.constData()) != 0) _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    m_fd = fd;
    m_pid = pid;
    m_exitCode = -1;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &GhosttyPty::readAvailable);
    Q_EMIT runningChanged();
    return true;
}

void GhosttyPty::write(const QVariant& data) {
    if (m_fd < 0) return;
    const QByteArray bytes = data.metaType().id() == QMetaType::QByteArray ? data.toByteArray()
                                                                           : data.toString().toUtf8();
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(m_fd, bytes.constData() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && errno == EAGAIN) {
            // The line discipline is full; wait for the child to drain it.
            fd_set set;
            FD_ZERO(&set);
            FD_SET(m_fd, &set);
            select(m_fd + 1, nullptr, &set, nullptr, nullptr);
        } else {
            return;
        }
    }
}

void GhosttyPty::resize(int cols, int rows) {
    if (m_fd < 0) return;
    struct winsize size = {};
    size.ws_col = static_cast<unsigned short>(qBound(1, cols, 65535));
    size.ws_row = static_cast<unsigned short>(qBound(1, rows, 65535));
    ioctl(m_fd, TIOCSWINSZ, &size);
}

void GhosttyPty::terminate() {
    if (m_pid > 0) kill(static_cast<pid_t>(m_pid), SIGHUP);
}

void GhosttyPty::readAvailable() {
    char buffer[65536];
    for (;;) {
        const ssize_t count = ::read(m_fd, buffer, sizeof buffer);
        if (count > 0) {
            Q_EMIT output(QByteArray(buffer, count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) return;
        // EOF, or EIO once the child and its descendants closed the slave.
        reap();
        return;
    }
}

void GhosttyPty::reap() {
    if (m_notifier != nullptr) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_pid <= 0) return;
    int status = 0;
    pid_t result;
    do {
        result = waitpid(static_cast<pid_t>(m_pid), &status, 0);
    } while (result < 0 && errno == EINTR);
    m_pid = 0;
    m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    Q_EMIT runningChanged();
    Q_EMIT exited(m_exitCode);
}

#else

bool GhosttyPty::start(int, int) {
    Q_EMIT errorOccurred(QStringLiteral("Pty is only available on Unix"));
    return false;
}
void GhosttyPty::write(const QVariant&) {}
void GhosttyPty::resize(int, int) {}
void GhosttyPty::terminate() {}
void GhosttyPty::readAvailable() {}
void GhosttyPty::reap() {}

#endif
