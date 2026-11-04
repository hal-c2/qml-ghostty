#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QtQml/QQmlExtensionPlugin>
#include <atomic>

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

    // Puts the terminal in the window, focused, with its padding gone from the sums.
    bool show(QQuickWindow& window, QQuickItem* term) {
        window.resize(800, 400);
        term->setParentItem(window.contentItem());
        window.show();
        if (!QTest::qWaitForWindowExposed(&window)) return false;
        term->forceActiveFocus();
        return term->hasActiveFocus();
    }

    static void write(QQuickItem* term, const QString& data) {
        QMetaObject::invokeMethod(term, "write", Q_ARG(QVariant, data));
    }

    // The middle of a cell, in window coordinates.
    static QPoint cell(QQuickItem* term, int column, int row) {
        const qreal padding = term->property("padding").toReal();
        return (term->mapToScene(QPointF(padding + term->property("cellWidth").toReal() * (column + 0.5),
                                         padding + term->property("cellHeight").toReal() * (row + 0.5))))
            .toPoint();
    }

    static QString selected(QQuickItem* term) {
        QString text;
        QMetaObject::invokeMethod(term, "selectedText", Q_RETURN_ARG(QString, text));
        return text;
    }

    static QString sent(const QSignalSpy& spy) {
        QString all;
        for (const auto& args : spy) all += args.at(0).toString();
        return all;
    }

    static void history(QQuickItem* term) {
        QString lines;
        for (int i = 0; i < 200; ++i) lines += QStringLiteral("line %1\r\n").arg(i);
        write(term, lines);
    }

    // How many frames the window starts in this long, with nothing else asking.
    static int framesOver(QQuickWindow& window, int milliseconds) {
        QTest::qWait(50);
        std::atomic<int> frames = 0;
        const auto connection = QObject::connect(&window, &QQuickWindow::beforeSynchronizing, &window,
                                                 [&frames] { ++frames; }, Qt::DirectConnection);
        QTest::qWait(milliseconds);
        QObject::disconnect(connection);
        return frames;
    }

    // Clicks count as repeats however loaded the machine is.
    static auto slowClicksStillRepeat() {
        QGuiApplication::styleHints()->setMouseDoubleClickInterval(60000);
        return qScopeGuard([] { QGuiApplication::styleHints()->setMouseDoubleClickInterval(-1); });
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

    // GNOME with blinking off reports a flash time of 0.
    void noFlashTimeMeansNoBlinking() {
        QGuiApplication::styleHints()->setCursorFlashTime(0);
        const auto restore = qScopeGuard([] { QGuiApplication::styleHints()->setCursorFlashTime(-1); });
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[1 q"));
        QVERIFY2(framesOver(window, 300) <= 1, "the cursor keeps repainting");
        // Typing restarts the blink, which must not start it here either.
        QTest::keyClick(&window, Qt::Key_A);
        QVERIFY2(framesOver(window, 300) <= 1, "typing set the cursor repainting");
    }

    void flashTimeTurnedOffStopsTheBlink() {
        QGuiApplication::styleHints()->setCursorFlashTime(100);
        const auto restore = qScopeGuard([] { QGuiApplication::styleHints()->setCursorFlashTime(-1); });
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[1 q"));
        QVERIFY2(framesOver(window, 300) > 0, "the cursor does not blink");
        QGuiApplication::styleHints()->setCursorFlashTime(0);
        QVERIFY2(framesOver(window, 300) <= 1, "the cursor keeps repainting");
    }

    void cursorBlinkOffKeepsTheCursorSteady() {
        QGuiApplication::styleHints()->setCursorFlashTime(100);
        const auto restore = qScopeGuard([] { QGuiApplication::styleHints()->setCursorFlashTime(-1); });
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[1 q"));
        QVERIFY2(framesOver(window, 300) > 0, "the cursor does not blink");
        QVERIFY(term->setProperty("cursorBlink", false));
        const qreal cellHeight = term->property("cellHeight").toReal();
        const qreal padding = term->property("padding").toReal();
        QTRY_VERIFY(window.grabWindow().pixelColor(int(padding + 2), int(padding + cellHeight / 2)) !=
                    term->property("backgroundColor").value<QColor>());
        QVERIFY2(framesOver(window, 300) <= 1, "a steady cursor keeps repainting");
        QVERIFY2(window.grabWindow().pixelColor(int(padding + 2), int(padding + cellHeight / 2)) !=
                     term->property("backgroundColor").value<QColor>(),
                 "the steady cursor is not drawn");
    }

    void dragReportsMotionWithItsButton() {
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[?1002h\x1b[?1006h"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 1, 0));
        QTest::mouseMove(&window, cell(term.get(), 4, 0));
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 4, 0));
        QCOMPARE(sent(spy), QStringLiteral("\x1b[<0;2;1M\x1b[<32;5;1M\x1b[<0;5;1m"));
        QVERIFY(!term->property("hasSelection").toBool());

        spy.clear();
        QTest::mousePress(&window, Qt::RightButton, Qt::NoModifier, cell(term.get(), 1, 0));
        QTest::mouseMove(&window, cell(term.get(), 3, 0));
        QTest::mouseRelease(&window, Qt::RightButton, Qt::NoModifier, cell(term.get(), 3, 0));
        QVERIFY2(sent(spy).contains(QLatin1String("\x1b[<34;4;1M")), qPrintable(sent(spy)));
    }

    void hoverReportsOnlyToAnyEventTracking() {
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[?1002h\x1b[?1006h"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::mouseMove(&window, cell(term.get(), 2, 0));
        QTest::mouseMove(&window, cell(term.get(), 3, 0));
        QCOMPARE(sent(spy), QString());
        write(term.get(), QStringLiteral("\x1b[?1003h"));
        QTest::mouseMove(&window, cell(term.get(), 5, 0));
        QVERIFY2(sent(spy).contains(QLatin1String("\x1b[<35;6;1M")), qPrintable(sent(spy)));
    }

    void doubleClickReportsTwoPresses() {
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[?1000h\x1b[?1006h"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 1, 0));
        QCOMPARE(sent(spy), QStringLiteral("\x1b[<0;2;1M\x1b[<0;2;1m\x1b[<0;2;1M\x1b[<0;2;1m"));
    }

    void clicksSelectAWordThenALine() {
        const auto repeat = slowClicksStillRepeat();
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("git commit -m fix\r\nnext"));
        const QPoint commit = cell(term.get(), 5, 0);
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, commit);
        QCOMPARE(selected(term.get()), QStringLiteral("commit"));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, commit);
        QCOMPARE(selected(term.get()), QStringLiteral("git commit -m fix"));
    }

    void slowClicksSelectNothing() {
        QGuiApplication::styleHints()->setMouseDoubleClickInterval(1);
        const auto restore = qScopeGuard([] { QGuiApplication::styleHints()->setMouseDoubleClickInterval(-1); });
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("git commit -m fix"));
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 5, 0));
        QTest::qWait(20);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 5, 0));
        QVERIFY(!term->property("hasSelection").toBool());
    }

    void clickingAwayClearsTheSelection() {
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("git commit -m fix"));
        QMetaObject::invokeMethod(term.get(), "selectAll");
        QVERIFY(term->property("hasSelection").toBool());
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 5, 0));
        QVERIFY(!term->property("hasSelection").toBool());
    }

    void holdingPastTheTopKeepsScrolling() {
        QQuickWindow window;
        auto term = create("y: 50");
        QVERIFY(show(window, term.get()));
        history(term.get());
        QMetaObject::invokeMethod(term.get(), "scrollTo", Q_ARG(qreal, 100));
        const int anchor = term->property("scrollOffset").toInt() + 2;
        // Past the text, so the anchor's whole line is selected.
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 20, 2));
        QTest::mouseMove(&window, cell(term.get(), 0, 1));
        QTest::mouseMove(&window, QPoint(cell(term.get(), 0, 1).x(), 20));
        // The pointer is still; the history keeps coming.
        QTRY_VERIFY(term->property("scrollOffset").toInt() <= 90);
        QStringList lines = selected(term.get()).split(QLatin1Char('\n'));
        QCOMPARE(lines.last(), QStringLiteral("line %1").arg(anchor));
        QCOMPARE(lines.size(), anchor - term->property("scrollOffset").toInt() + 1);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(cell(term.get(), 0, 1).x(), 20));
        const int stopped = term->property("scrollOffset").toInt();
        QTest::qWait(150);
        QCOMPARE(term->property("scrollOffset").toInt(), stopped);
        lines = selected(term.get()).split(QLatin1Char('\n'));
        QCOMPARE(lines.last(), QStringLiteral("line %1").arg(anchor));
    }

    void holdingPastTheBottomKeepsScrolling() {
        QQuickWindow window;
        auto term = create();
        term->setHeight(300);
        QVERIFY(show(window, term.get()));
        history(term.get());
        QMetaObject::invokeMethod(term.get(), "scrollTo", Q_ARG(qreal, 100));
        const int anchor = term->property("scrollOffset").toInt() + 2;
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 0, 2));
        QTest::mouseMove(&window, cell(term.get(), 0, 3));
        QTest::mouseMove(&window, QPoint(cell(term.get(), 0, 3).x(), 350));
        QTRY_VERIFY(term->property("scrollOffset").toInt() >= 110);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(cell(term.get(), 0, 3).x(), 350));
        QCOMPARE(selected(term.get()).split(QLatin1Char('\n')).first(), QStringLiteral("line %1").arg(anchor));
    }

    void holdingAtTheEndOfTheHistoryPaintsNothing() {
        QQuickWindow window;
        auto term = create("y: 50");
        QVERIFY(show(window, term.get()));
        history(term.get());
        QMetaObject::invokeMethod(term.get(), "scrollTo", Q_ARG(qreal, 3));
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 20, 2));
        QTest::mouseMove(&window, cell(term.get(), 20, 1));
        QTest::mouseMove(&window, QPoint(cell(term.get(), 20, 1).x(), 20));
        QTRY_COMPARE(term->property("scrollOffset").toInt(), 0);
        QVERIFY2(framesOver(window, 300) <= 1, "holding at the end of the history keeps repainting");
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(cell(term.get(), 20, 1).x(), 20));
    }

    void aScreenChangeEndsTheDrag() {
        QQuickWindow window;
        auto term = create("y: 50");
        QVERIFY(show(window, term.get()));
        history(term.get());
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 0, 2));
        QTest::mouseMove(&window, cell(term.get(), 6, 2));
        const QString before = selected(term.get());
        QVERIFY(!before.isEmpty());
        // A full-screen program takes over while the button is down.
        write(term.get(), QStringLiteral("\x1b[?1049h"));
        QTest::mouseMove(&window, QPoint(cell(term.get(), 6, 2).x(), 20));
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, QPoint(cell(term.get(), 6, 2).x(), 20));
        write(term.get(), QStringLiteral("\x1b[?1049l"));
        QMetaObject::invokeMethod(term.get(), "reset");
        QVERIFY(!term->property("hasSelection").toBool());
    }

    void resetDuringADragIsSafe() {
        QQuickWindow window;
        auto term = create("y: 50");
        QVERIFY(show(window, term.get()));
        history(term.get());
        QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 0, 2));
        QTest::mouseMove(&window, QPoint(cell(term.get(), 6, 2).x(), 20));
        QMetaObject::invokeMethod(term.get(), "reset");
        QTest::mouseMove(&window, cell(term.get(), 6, 2));
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, cell(term.get(), 6, 2));
        QVERIFY(!term->property("hasSelection").toBool());
        QCOMPARE(term->property("scrollOffset").toInt(), 0);
    }

    void modifiersOverrideTheProgramsMouse_data() {
        QTest::addColumn<Qt::KeyboardModifiers>("modifier");
        QTest::newRow("Shift") << Qt::KeyboardModifiers(Qt::ShiftModifier);
        QTest::newRow("Control") << Qt::KeyboardModifiers(Qt::ControlModifier);
        QTest::newRow("Meta") << Qt::KeyboardModifiers(Qt::MetaModifier);
    }

    void modifiersOverrideTheProgramsMouse() {
        QFETCH(Qt::KeyboardModifiers, modifier);
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("selected text\x1b[?1002h\x1b[?1006h"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::mousePress(&window, Qt::LeftButton, modifier, cell(term.get(), 0, 0));
        QTest::mouseMove(&window, cell(term.get(), 9, 0));
        QTest::mouseRelease(&window, Qt::LeftButton, modifier, cell(term.get(), 9, 0));
        QVERIFY2(selected(term.get()).startsWith(QLatin1String("selected")), qPrintable(selected(term.get())));
        QCOMPARE(sent(spy), QString());
    }

    void drawsTheComposition() {
        QQuickWindow window;
        auto term = create("backgroundColor: \"#000000\"; padding: 0");
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("$ \x1b[?25l"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        const QRect area(cell(term.get(), 2, 0) - QPoint(term->property("cellWidth").toInt() / 2, term->property("cellHeight").toInt() / 2),
                         QSize(term->property("cellWidth").toInt() * 6, term->property("cellHeight").toInt()));
        const auto inks = [&] {
            const QImage frame = window.grabWindow();
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x)
                    if (frame.pixelColor(x, y) != QColor(Qt::black)) return true;
            return false;
        };
        QVERIFY(!inks());
        QInputMethodEvent composing(QStringLiteral("abc"), {});
        QCoreApplication::sendEvent(term.get(), &composing);
        QTRY_VERIFY2(inks(), "the composition is not drawn");
        QCOMPARE(spy.count(), 0);
        QInputMethodEvent cancelled;
        QCoreApplication::sendEvent(term.get(), &cancelled);
        QTRY_VERIFY2(!inks(), "the composition stays after it ended");

        QInputMethodEvent again(QStringLiteral("abc"), {});
        QCoreApplication::sendEvent(term.get(), &again);
        QInputMethodEvent committed;
        committed.setCommitString(QStringLiteral("ABC"));
        QCoreApplication::sendEvent(term.get(), &committed);
        QCOMPARE(sent(spy), QStringLiteral("ABC"));
        QTRY_VERIFY2(!inks(), "the composition stays after its commit");
    }

    void compositionComesBackToThePrompt() {
        QQuickWindow window;
        auto term = create();
        QVERIFY(show(window, term.get()));
        history(term.get());
        QMetaObject::invokeMethod(term.get(), "scrollLines", Q_ARG(int, -10));
        QInputMethodEvent composing(QStringLiteral("abc"), {});
        QCoreApplication::sendEvent(term.get(), &composing);
        QVERIFY(term->property("atBottom").toBool());
    }

    // Qt calls macOS's Ctrl Meta, and its Cmd Control.
    void macModifiersEncodeCtrlForThePrograms() {
        QQuickWindow window;
        auto term = create("macModifiers: true");
        QVERIFY(term);
        QVERIFY(show(window, term.get()));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::keyClick(&window, Qt::Key_C, Qt::MetaModifier);
        QCOMPARE(sent(spy), QStringLiteral("\x03"));
        write(term.get(), QStringLiteral("\x1b[>1u"));
        spy.clear();
        QTest::keyClick(&window, Qt::Key_C, Qt::MetaModifier);
        QCOMPARE(sent(spy), QStringLiteral("\x1b[99;5u"));
        spy.clear();
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(sent(spy), QStringLiteral("\x1b[99;9u"));
    }

    void otherPlatformsKeepTheirModifiers() {
        QQuickWindow window;
        auto term = create("macModifiers: false");
        QVERIFY(term);
        QVERIFY(show(window, term.get()));
        write(term.get(), QStringLiteral("\x1b[>1u"));
        QSignalSpy spy(term.get(), SIGNAL(input(QString)));
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(sent(spy), QStringLiteral("\x1b[99;5u"));
        spy.clear();
        QTest::keyClick(&window, Qt::Key_C, Qt::MetaModifier);
        QCOMPARE(sent(spy), QStringLiteral("\x1b[99;9u"));
    }

    void saysWhetherAProgramHasTheMouse() {
        auto term = create();
        QVERIFY(!term->property("mouseTracking").toBool());
        QSignalSpy spy(term.get(), SIGNAL(mouseTrackingChanged()));
        write(term.get(), QStringLiteral("\x1b[?1002h"));
        QVERIFY(term->property("mouseTracking").toBool());
        QCOMPARE(spy.count(), 1);
        write(term.get(), QStringLiteral("more output"));
        QCOMPARE(spy.count(), 1);
        write(term.get(), QStringLiteral("\x1b[?1002l"));
        QVERIFY(!term->property("mouseTracking").toBool());
        QCOMPARE(spy.count(), 2);
        write(term.get(), QStringLiteral("\x1b[?1000h"));
        QCOMPARE(spy.count(), 3);
        QMetaObject::invokeMethod(term.get(), "reset");
        QVERIFY(!term->property("mouseTracking").toBool());
        QCOMPARE(spy.count(), 4);
    }

    void aHealthyTerminalHasNoError() {
        auto term = create();
        QVERIFY(term->metaObject()->indexOfProperty("error") >= 0);
        QCOMPARE(term->property("error").toString(), QString());
    }

    void copySaysWhetherItCopied() {
        auto term = create();
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        bool copied = true;
        QVERIFY(QMetaObject::invokeMethod(term.get(), "copy", Q_RETURN_ARG(bool, copied)));
        QVERIFY(!copied);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("before"));
        write(term.get(), QStringLiteral("hello"));
        QMetaObject::invokeMethod(term.get(), "selectAll");
        QVERIFY(QMetaObject::invokeMethod(term.get(), "copy", Q_RETURN_ARG(bool, copied)));
        QVERIFY(copied);
        QCOMPARE(QGuiApplication::clipboard()->text(), selected(term.get()));
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
