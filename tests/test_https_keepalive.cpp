#ifndef QTNG_NO_CRYPTO

#include <atomic>
#include <QtTest>
#include "qtnetworkng.h"

using namespace qtng;

namespace {

enum class ServerMode {
    KeepConnectionOpen,
    CloseAfterKeepAliveResponseAfterDelay,
    CloseAfterKeepAliveResponseImmediately,
};

class KeepAliveTestHandler : public BaseHttpRequestHandler
{
public:
    static ServerMode mode;
    static std::atomic<int> requestCount;

    void doGET() override
    {
        ++requestCount;
        if (path != QLatin1String("/ping")) {
            sendError(HttpStatus::NotFound);
            return;
        }

        sendResponse(HttpStatus::OK);
        sendHeader("Content-Type", "text/plain");
        const QByteArray body("pong");
        sendHeader("Content-Length", QByteArray::number(body.size()));
        sendHeader("Connection", "keep-alive");
        if (!endHeader()) {
            return;
        }
        request->sendall(body);
        
        // 模拟 nginx keepalive_timeout：响应头声明 keep-alive，但服务端随后关闭 TLS。
        if (mode == ServerMode::CloseAfterKeepAliveResponseAfterDelay) {
            QWeakPointer<SocketLike> requestPtr = request;
            Coroutine::spawn([requestPtr] {
                Coroutine::msleep(500);
                QSharedPointer<SocketLike> request = requestPtr.toStrongRef();
                if (request.isNull()) {
                    return;
                }
                request->close();
            });
        } else if (mode == ServerMode::CloseAfterKeepAliveResponseImmediately) {
            request->close();
            closeConnection = Yes;
        }
    }
};

ServerMode KeepAliveTestHandler::mode = ServerMode::KeepConnectionOpen;
std::atomic<int> KeepAliveTestHandler::requestCount{0};

using KeepAliveHttpsServer = SslServer<KeepAliveTestHandler>;

static QString makePingUrl(quint16 port)
{
    return QString::fromLatin1("https://127.0.0.1:%1/ping").arg(port);
}

}  // namespace

class TestHttpsKeepAlive : public QObject
{
    Q_OBJECT
private slots:
    void testKeepAliveWorksWhenServerStaysOpen();
    void testStaleSslKeepAliveConnectionRecoversAfterDelay();
    void testStaleSslKeepAliveConnectionRecoversImmediately();
    void testStaleSslKeepAliveDisabledRecovers();
};

void TestHttpsKeepAlive::testKeepAliveWorksWhenServerStaysOpen()
{
    KeepAliveTestHandler::mode = ServerMode::KeepConnectionOpen;
    KeepAliveTestHandler::requestCount = 0;

    KeepAliveHttpsServer server(HostAddress::LocalHost, 0);
    QVERIFY(server.start());
    QVERIFY(server.started()->tryWait(2000));
    const quint16 port = server.serverPort();
    QVERIFY(port != 0);

    HttpSession session;
    session.setKeepAlive(true);
    session.sslConfiguration().setPeerVerifyMode(Ssl::VerifyNone);
    const QString url = makePingUrl(port);

    HttpResponse first = session.get(url);
    QVERIFY2(first.isOk(), qPrintable(first.error() ? first.error()->what() : QString()));
    QCOMPARE(first.body(), QByteArray("pong"));

    HttpResponse second = session.get(url);
    QVERIFY2(second.isOk(), qPrintable(second.error() ? second.error()->what() : QString()));
    QCOMPARE(second.body(), QByteArray("pong"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 2);

    server.stop();
    server.wait();
}

void TestHttpsKeepAlive::testStaleSslKeepAliveConnectionRecoversAfterDelay()
{
    KeepAliveTestHandler::mode = ServerMode::CloseAfterKeepAliveResponseAfterDelay;
    KeepAliveTestHandler::requestCount = 0;

    KeepAliveHttpsServer server(HostAddress::LocalHost, 0);
    QVERIFY(server.start());
    QVERIFY(server.started()->tryWait(2000));
    const quint16 port = server.serverPort();
    QVERIFY(port != 0);

    HttpSession session;
    session.setKeepAlive(true);
    session.setDebugLevel(1);
    session.sslConfiguration().setPeerVerifyMode(Ssl::VerifyNone);
    const QString url = makePingUrl(port);

    HttpResponse first = session.get(url);
    QVERIFY2(first.isOk(), qPrintable(first.error() ? first.error()->what() : QString()));
    QCOMPARE(first.body(), QByteArray("pong"));
    QCOMPARE(first.header(KnownHeader::ConnectionHeader).toLower(), QByteArray("keep-alive"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 1);

    Coroutine::msleep(1000);

    // 服务端已关闭 TLS，连接池应丢弃 stale 连接并新建连接。
    HttpResponse second = session.get(url);
    QVERIFY2(second.isOk(), qPrintable(second.error() ? second.error()->what() : QString()));
    QCOMPARE(second.body(), QByteArray("pong"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 2);

    server.stop();
    server.wait();
}

void TestHttpsKeepAlive::testStaleSslKeepAliveConnectionRecoversImmediately()
{
#if 0
    // TODO: should we do retry in HttpSession::send or SSL_peek and check close
#else
    KeepAliveTestHandler::mode = ServerMode::CloseAfterKeepAliveResponseImmediately;
    KeepAliveTestHandler::requestCount = 0;

    KeepAliveHttpsServer server(HostAddress::LocalHost, 0);
    QVERIFY(server.start());
    QVERIFY(server.started()->tryWait(2000));
    const quint16 port = server.serverPort();
    QVERIFY(port != 0);

    HttpSession session;
    session.setKeepAlive(true);
    session.setDebugLevel(1);
    session.sslConfiguration().setPeerVerifyMode(Ssl::VerifyNone);
    const QString url = makePingUrl(port);

    HttpResponse first = session.get(url);
    QVERIFY2(first.isOk(), qPrintable(first.error() ? first.error()->what() : QString()));
    QCOMPARE(first.body(), QByteArray("pong"));
    QCOMPARE(first.header(KnownHeader::ConnectionHeader).toLower(), QByteArray("keep-alive"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 1);

    // 服务端已关闭 TLS，连接池应丢弃 stale 连接并新建连接。
    HttpResponse second = session.get(url);
    QVERIFY2(second.isOk(), qPrintable(second.error() ? second.error()->what() : QString()));
    QCOMPARE(second.body(), QByteArray("pong"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 2);

    server.stop();
    server.wait();
#endif
}


void TestHttpsKeepAlive::testStaleSslKeepAliveDisabledRecovers()
{
    KeepAliveTestHandler::mode = ServerMode::CloseAfterKeepAliveResponseImmediately;
    KeepAliveTestHandler::requestCount = 0;

    KeepAliveHttpsServer server(HostAddress::LocalHost, 0);
    QVERIFY(server.start());
    QVERIFY(server.started()->tryWait(2000));
    const quint16 port = server.serverPort();
    QVERIFY(port != 0);

    HttpSession session;
    session.setKeepAlive(false);
    session.sslConfiguration().setPeerVerifyMode(Ssl::VerifyNone);
    const QString url = makePingUrl(port);

    HttpResponse first = session.get(url);
    QVERIFY2(first.isOk(), qPrintable(first.error() ? first.error()->what() : QString()));
    QCOMPARE(first.body(), QByteArray("pong"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 1);

    HttpResponse second = session.get(url);
    QVERIFY2(second.isOk(), qPrintable(second.error() ? second.error()->what() : QString()));
    QCOMPARE(second.body(), QByteArray("pong"));
    QCOMPARE(KeepAliveTestHandler::requestCount.load(), 2);

    server.stop();
    server.wait();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    int result = 0;
    QScopedPointer<Coroutine> runner(Coroutine::spawn([&result, argc, argv] {
        TestHttpsKeepAlive test;
        result += QTest::qExec(&test, argc, argv);
        callInEventLoop([] {
            if (QCoreApplication::instance()) {
                QCoreApplication::instance()->quit();
            }
        });
    }));
    startQtLoop();
    return result;
}

#include "test_https_keepalive.moc"

#endif  // QTNG_NO_CRYPTO
