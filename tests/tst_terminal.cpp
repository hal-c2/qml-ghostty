#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>

Q_IMPORT_QML_PLUGIN(GhosttyPlugin)

class TerminalTest : public QObject {
    Q_OBJECT

private:
    QQmlEngine m_engine;

    std::unique_ptr<QQuickItem> create(const QByteArray& properties = {}) {
        QQmlComponent component(&m_engine);
        component.setData("import Ghostty\nTerminal { width: 800; height: 400; " + properties + " }", QUrl());
        auto* item = qobject_cast<QQuickItem*>(component.create());
        if (item == nullptr) qWarning() << component.errors();
        return std::unique_ptr<QQuickItem>(item);
    }

private slots:
    void writesText() {
        auto term = create();
        QVERIFY(term);
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("hello\r\nworld")));
        QString text;
        QMetaObject::invokeMethod(term.get(), "text", Q_RETURN_ARG(QString, text));
        QCOMPARE(text.trimmed(), QStringLiteral("hello\nworld"));
    }

    void acceptsBytes() {
        auto term = create();
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QByteArray("caf\xc3\xa9")));
        QString text;
        QMetaObject::invokeMethod(term.get(), "text", Q_RETURN_ARG(QString, text));
        QCOMPARE(text.trimmed(), QStringLiteral("café"));
    }

    void setsTitle() {
        auto term = create();
        QSignalSpy spy(term.get(), SIGNAL(titleChanged()));
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("\x1b]2;my title\x07")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(term->property("title").toString(), QStringLiteral("my title"));
    }

    void answersQueries() {
        auto term = create();
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("ab\x1b[6n")));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("\x1b[1;3R"));
    }

    void restoreStaysQuiet() {
        auto term = create();
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QMetaObject::invokeMethod(term.get(), "restore", Q_ARG(QVariant, QStringLiteral("old\x1b[6n\x1b[c")));
        QCOMPARE(spy.count(), 0);
        QString text;
        QMetaObject::invokeMethod(term.get(), "text", Q_RETURN_ARG(QString, text));
        QCOMPARE(text.trimmed(), QStringLiteral("old"));
    }

    void resizesGrid() {
        auto term = create();
        const int columns = term->property("columns").toInt();
        QVERIFY(columns > 10);
        QSignalSpy spy(term.get(), SIGNAL(resized(int, int)));
        term->setWidth(term->width() / 2);
        QCOMPARE(spy.count(), 1);
        QVERIFY(term->property("columns").toInt() < columns);
        QCOMPARE(spy.at(0).at(0).toInt(), term->property("columns").toInt());
    }

    void encodesKeys() {
        QQuickWindow window;
        window.resize(800, 400);
        auto term = create();
        term->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        term->forceActiveFocus();
        QVERIFY(term->hasActiveFocus());

        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::keyClick(&window, Qt::Key_A, Qt::NoModifier);
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&window, Qt::Key_Return);
        QTest::keyClick(&window, Qt::Key_Up);
        QStringList sent;
        for (const auto& args : spy) sent << args.at(0).toString();
        QCOMPARE(sent, (QStringList{"a", "\x03", "\r", "\x1b[A"}));

        // Application cursor keys switch arrows to SS3.
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("\x1b[?1h")));
        spy.clear();
        QTest::keyClick(&window, Qt::Key_Up);
        QCOMPARE(spy.value(0).value(0).toString(), QStringLiteral("\x1bOA"));
        term->setParentItem(nullptr);
    }

    void bracketsPaste() {
        auto term = create();
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QMetaObject::invokeMethod(term.get(), "paste", Q_ARG(QString, QStringLiteral("ls")));
        QCOMPARE(spy.value(0).value(0).toString(), QStringLiteral("ls"));
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("\x1b[?2004h")));
        spy.clear();
        QMetaObject::invokeMethod(term.get(), "paste", Q_ARG(QString, QStringLiteral("ls")));
        QCOMPARE(spy.value(0).value(0).toString(), QStringLiteral("\x1b[200~ls\x1b[201~"));
    }

    void selectsAll() {
        auto term = create();
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, QStringLiteral("one\r\ntwo")));
        QMetaObject::invokeMethod(term.get(), "selectAll");
        QVERIFY(term->property("hasSelection").toBool());
        QString selected;
        QMetaObject::invokeMethod(term.get(), "selectedText", Q_RETURN_ARG(QString, selected));
        QCOMPARE(selected.trimmed(), QStringLiteral("one\ntwo"));
        QMetaObject::invokeMethod(term.get(), "clearSelection");
        QVERIFY(!term->property("hasSelection").toBool());
    }

    void scrollsHistory() {
        auto term = create();
        QString lines;
        for (int i = 0; i < 200; ++i) lines += QStringLiteral("line %1\r\n").arg(i);
        QMetaObject::invokeMethod(term.get(), "write", Q_ARG(QVariant, lines));
        QVERIFY(term->property("atBottom").toBool());
        QVERIFY(term->property("scrollTotal").toReal() > term->property("rows").toReal());
        QMetaObject::invokeMethod(term.get(), "scrollLines", Q_ARG(int, -10));
        QVERIFY(!term->property("atBottom").toBool());
        QMetaObject::invokeMethod(term.get(), "scrollToBottom");
        QVERIFY(term->property("atBottom").toBool());
    }

    void paints() {
        QQuickWindow window;
        window.resize(640, 200);
        auto term = create("backgroundColor: \"#000000\"; padding: 0");
        term->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QMetaObject::invokeMethod(
            term.get(), "write",
            Q_ARG(QVariant, QStringLiteral("\x1b[41m  \x1b[0m plain \x1b[1;32mbold green\x1b[0m \x1b[4munder\x1b[0m "
                                           "\u00e9\u4e2d\U0001F600\r\n\x1b[7mreverse\x1b[0m\r\n$ ")));
        const QImage frame = window.grabWindow();
        if (qEnvironmentVariableIsSet("GHOSTTY_TEST_FRAME")) frame.save(qEnvironmentVariable("GHOSTTY_TEST_FRAME"));
        const qreal cellHeight = term->property("cellHeight").toReal();
        const qreal cellWidth = term->property("cellWidth").toReal();
        const qreal dpr = frame.devicePixelRatio();
        // The first two cells have a red background; past the text is black.
        QCOMPARE(frame.pixelColor(int(cellWidth * dpr), int(cellHeight / 2 * dpr)).red() > 150, true);
        QCOMPARE(frame.pixelColor(frame.width() - 2, frame.height() - 2), QColor(Qt::black));
        term->setParentItem(nullptr);
    }

    void runsProgram() {
        QQmlComponent component(&m_engine);
        component.setData(R"(
            import Ghostty
            Terminal {
                id: term
                width: 800; height: 400
                property alias pty: pty
                Pty {
                    id: pty
                    program: "/bin/sh"
                    arguments: ["-c", "stty size; read line; echo got:$line"]
                    onOutput: data => term.write(data)
                }
                onInput: data => pty.write(data)
            }
        )", QUrl());
        std::unique_ptr<QQuickItem> term(qobject_cast<QQuickItem*>(component.create()));
        QVERIFY2(term, qPrintable(component.errorString()));
        auto* pty = term->property("pty").value<QObject*>();
        QSignalSpy exited(pty, SIGNAL(exited(int)));
        bool started = false;
        QMetaObject::invokeMethod(pty, "start", Q_RETURN_ARG(bool, started),
                                  Q_ARG(int, term->property("columns").toInt()),
                                  Q_ARG(int, term->property("rows").toInt()));
        QVERIFY(started);
        const QString size = QStringLiteral("%1 %2").arg(term->property("rows").toInt()).arg(term->property("columns").toInt());
        const auto text = [&] {
            QString value;
            QMetaObject::invokeMethod(term.get(), "text", Q_RETURN_ARG(QString, value));
            return value;
        };
        QTRY_VERIFY2(text().contains(size), qPrintable(text()));
        QMetaObject::invokeMethod(term.get(), "paste", Q_ARG(QString, QStringLiteral("hi\r")));
        QTRY_COMPARE(exited.count(), 1);
        QVERIFY2(text().contains(QStringLiteral("got:hi")), qPrintable(text()));
        QCOMPARE(exited.at(0).at(0).toInt(), 0);
    }
};

QTEST_MAIN(TerminalTest)
#include "tst_terminal.moc"
