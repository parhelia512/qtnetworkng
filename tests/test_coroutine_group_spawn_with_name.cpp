#include <QtTest>
#include "qtnetworkng.h"

using namespace qtng;

// 模拟持有 CoroutineGroup 的父对象（类似 NotificationsPagePrivate）
class CoroutineGroupTest : public QObject
{
    Q_OBJECT
public:
    CoroutineGroupTest()
        : operations(new CoroutineGroup())
    {
        connect(this, &CoroutineGroupTest::emitTask, this, &CoroutineGroupTest::doTaskAsync, Qt::QueuedConnection);
    }
    void doTaskAsync(bool replace)
    {
        operations->spawnWithName("task", [] {
            Coroutine::msleep(500);
        }, replace);
    }
    void startLongTask()
    {
        operations->spawnWithName("task", [] {
            Coroutine::msleep(500);
        });
    }
    QSharedPointer<Coroutine> replaceTask()
    {
        return operations->spawnWithName("task", [] {
            Coroutine::msleep(50);
        }, true);
    }
    void fun()
    {
        emit emitTask(true);
    }
    ~CoroutineGroupTest()
    {
        delete operations;
    }
signals:
    void emitTask(bool replace);
private:
    CoroutineGroup *operations;
};

class TestCoroutineGroupSpawnWithName : public QObject
{
    Q_OBJECT
private slots:
    void testSpawnWithNameReplace();
    void testSpawnWithNameReplaceWhileParentDestruct();
    void testSpawnWithNameReplaceWhileParentDestructFromGroupCoroutine();
    void testSpawnWithNameReturnsNullWhenGroupDestroyedDuringJoin();
};

void TestCoroutineGroupSpawnWithName::testSpawnWithNameReplace()
{
    CoroutineGroupTest parent;
    QSharedPointer<Event> finished(new Event());

    parent.doTaskAsync(false);
    parent.doTaskAsync(true);
    Coroutine::msleep(500);
}

void TestCoroutineGroupSpawnWithName::testSpawnWithNameReplaceWhileParentDestruct()
{
    // spawnWithName(..., replace=true) 会在 old->join() 处阻塞；
    // 若 join 期间父对象析构并 delete operations，CoroutineGroup 可能在
    // spawnWithName 仍在执行栈上时被销毁，导致 use-after-free。

    QList<int> tasks;
    for (int i = 0; i < 500; i++) {
        tasks.push_back(i);
    }
    CoroutineGroup::each<int>([] (int i) {
        CoroutineGroupTest *parent = new CoroutineGroupTest();
        for (int j = 0; j < 10; j++) {
            parent->doTaskAsync(true);
        }
        Coroutine::msleep(500);
        delete parent;
    }, tasks);
}

void TestCoroutineGroupSpawnWithName::testSpawnWithNameReplaceWhileParentDestructFromGroupCoroutine()
{
    // 更接近真实用法：由组内协程调用 spawnWithName(..., replace=true)，
    // 同时在 join 阻塞期间析构持有 CoroutineGroup 的父对象。
    QList<int> tasks;
    for (int i = 0; i < 500; i++) {
        tasks.push_back(i);
    }
    CoroutineGroup::each<int>([] (int i) {
        CoroutineGroupTest *parent = new CoroutineGroupTest();
        parent->doTaskAsync(false);
        Coroutine::spawn([parent] {
            Coroutine::msleep(0);
            delete parent;
        });
        for (int i = 0; i < 100; i++) {
            parent->fun();
        }
    }, tasks);
}

void TestCoroutineGroupSpawnWithName::testSpawnWithNameReturnsNullWhenGroupDestroyedDuringJoin()
{
    bool hitNullReturn = false;
    QList<int> tasks;
    for (int i = 0; i < 200; i++) {
        tasks.push_back(i);
    }
    CoroutineGroup::each<int>([&hitNullReturn] (int i) {
        if (hitNullReturn) {
            return;
        }
        CoroutineGroupTest *parent = new CoroutineGroupTest();
        parent->startLongTask();
        Coroutine::msleep(1);

        Coroutine::spawn([parent] {
            delete parent;
        });

        QSharedPointer<Coroutine> result = parent->replaceTask();
        if (result.isNull()) {
            hitNullReturn = true;
        }
    }, tasks);
    QVERIFY(!hitNullReturn);
}

int main(int argc, char **argv)
{
    qSetMessagePattern("[%{type}][%{time MM-dd hh:mm:ss:zzz}]%{category}: %{message}");
    QCoreApplication app(argc, argv);

    int result = 0;
    QScopedPointer<Coroutine> runner(Coroutine::spawn([&result, argc, argv] {
        TestCoroutineGroupSpawnWithName test;
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

#include "test_coroutine_group_spawn_with_name.moc"
