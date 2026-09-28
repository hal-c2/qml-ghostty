#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariant>

class QSocketNotifier;

// A local pseudo terminal running one program, for using Terminal without a
// remote backend:
//
//   Pty { id: pty; onOutput: data => term.write(data) }
//   Terminal { id: term; onInput: data => pty.write(data)
//              onResized: (cols, rows) => pty.resize(cols, rows) }
//
// Unix only; elsewhere start() reports an error.
class GhosttyPty : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Pty)

    // Empty means $SHELL, then /bin/sh.
    Q_PROPERTY(QString program READ program WRITE setProgram NOTIFY programChanged)
    Q_PROPERTY(QStringList arguments READ arguments WRITE setArguments NOTIFY argumentsChanged)
    Q_PROPERTY(QString workingDirectory READ workingDirectory WRITE setWorkingDirectory NOTIFY workingDirectoryChanged)
    // Extra NAME=value entries on top of the inherited environment.
    Q_PROPERTY(QStringList environment READ environment WRITE setEnvironment NOTIFY environmentChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int exitCode READ exitCode NOTIFY exited)

public:
    explicit GhosttyPty(QObject* parent = nullptr);
    ~GhosttyPty() override;

    QString program() const { return m_program; }
    void setProgram(const QString& program);
    QStringList arguments() const { return m_arguments; }
    void setArguments(const QStringList& arguments);
    QString workingDirectory() const { return m_workingDirectory; }
    void setWorkingDirectory(const QString& directory);
    QStringList environment() const { return m_environment; }
    void setEnvironment(const QStringList& environment);
    bool running() const { return m_pid > 0; }
    int exitCode() const { return m_exitCode; }

    // Starts at the given size; returns false and emits errorOccurred on failure.
    Q_INVOKABLE bool start(int cols = 80, int rows = 24);
    // Accepts a string (sent as UTF-8) or an ArrayBuffer.
    Q_INVOKABLE void write(const QVariant& data);
    Q_INVOKABLE void resize(int cols, int rows);
    // SIGHUP, as a closing terminal would.
    Q_INVOKABLE void terminate();

signals:
    void programChanged();
    void argumentsChanged();
    void workingDirectoryChanged();
    void environmentChanged();
    void runningChanged();
    void output(const QByteArray& data);
    void exited(int exitCode);
    void errorOccurred(const QString& message);

private:
    void readAvailable();
    void reap();

    QString m_program;
    QStringList m_arguments;
    QString m_workingDirectory;
    QStringList m_environment;
    int m_fd = -1;
    qint64 m_pid = 0;
    int m_exitCode = -1;
    QSocketNotifier* m_notifier = nullptr;
};
