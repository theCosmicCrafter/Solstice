/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <testflake.h>

#include <KoCanvasBase.h>
#include <KoCanvasController.h>
#include <KoSelectedShapesProxySimple.h>
#include <KoShapeControllerBase.h>
#include <KoShapeManager.h>
#include <KoShapeRegistry.h>
#include <KoToolBase.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolProxy.h>
#include <KoToolRegistry.h>
#include <KoUnit.h>
#include <KoViewConverter.h>
#include <KoZoomState.h>
#include <kactioncollection.h>

#include <QSignalSpy>
#include <QWidget>
#include <memory>

namespace
{
const QString brushId = QStringLiteral("TestKoToolManager/Brush");
const QString transformId = QStringLiteral("TestKoToolManager/Transform");

class TestDocument : public KoShapeControllerBase
{
public:
    QRectF documentRectInPixels() const override
    {
        return QRectF(0, 0, 100, 100);
    }
    qreal pixelsPerInch() const override
    {
        return 72;
    }
};

class TestProxy : public KoToolProxy
{
public:
    explicit TestProxy(KoCanvasBase *canvas)
        : KoToolProxy(canvas)
    {
    }
    QPointF widgetToDocument(const QPointF &point) const override
    {
        return point;
    }
    QPointF documentToWidget(const QPointF &point) const override
    {
        return point;
    }
};

class TestCanvas : public KoCanvasBase
{
public:
    explicit TestCanvas(TestDocument *document)
        : KoCanvasBase(document)
        , manager(this)
        , selected(&manager)
        , proxy(this)
    {
    }

    void gridSize(QPointF *, QSizeF *) const override
    {
    }
    bool snapToGrid() const override
    {
        return false;
    }
    void addCommand(KUndo2Command *) override
    {
    }
    KoShapeManager *shapeManager() const override
    {
        return const_cast<KoShapeManager *>(&manager);
    }
    KoSelectedShapesProxy *selectedShapesProxy() const override
    {
        return const_cast<KoSelectedShapesProxySimple *>(&selected);
    }
    void updateCanvas(const QRectF &) override
    {
    }
    KoToolProxy *toolProxy() const override
    {
        return const_cast<TestProxy *>(&proxy);
    }
    const KoViewConverter *viewConverter() const override
    {
        return &converter;
    }
    KoViewConverter *viewConverter() override
    {
        return &converter;
    }
    QWidget *canvasWidget() override
    {
        return &widget;
    }
    const QWidget *canvasWidget() const override
    {
        return &widget;
    }
    KoUnit unit() const override
    {
        return KoUnit(KoUnit::Point);
    }
    void setCursor(const QCursor &) override
    {
    }

    QWidget widget;
    KoShapeManager manager;
    KoSelectedShapesProxySimple selected;
    TestProxy proxy;
    KoViewConverter converter;
};

class TestController : public KoCanvasController
{
public:
    TestController(TestCanvas *canvas, KisKActionCollection *actions)
        : KoCanvasController(actions)
        , m_canvas(canvas)
    {
    }
    KoCanvasBase *canvas() const override
    {
        return m_canvas;
    }
    void setCanvas(KoCanvasBase *) override
    {
    }
    void ensureVisibleDoc(const QRectF &, bool) override
    {
    }
    void zoomIn(const KoViewTransformStillPoint &) override
    {
    }
    void zoomIn() override
    {
    }
    void zoomOut(const KoViewTransformStillPoint &) override
    {
    }
    void zoomOut() override
    {
    }
    void zoomTo(const QRect &) override
    {
    }
    void setZoom(KoZoomMode::Mode, qreal) override
    {
    }
    void setPreferredCenter(const QPointF &) override
    {
    }
    QPointF preferredCenter() const override
    {
        return {};
    }
    void pan(const QPoint &) override
    {
    }
    void panUp() override
    {
    }
    void panDown() override
    {
    }
    void panLeft() override
    {
    }
    void panRight() override
    {
    }
    QPoint scrollBarValue() const override
    {
        return {};
    }
    void setScrollBarValue(const QPoint &) override
    {
    }
    void resetScrollBars() override
    {
    }
    QPointF currentCursorPosition() const override
    {
        return {};
    }
    KoZoomState zoomState() const override
    {
        return {};
    }

private:
    TestCanvas *m_canvas;
};

class TestTool : public KoToolBase
{
public:
    explicit TestTool(KoCanvasBase *canvas)
        : KoToolBase(canvas)
    {
    }
    void paint(QPainter &, const KoViewConverter &) override
    {
    }
    void mousePressEvent(KoPointerEvent *) override
    {
    }
    void mouseMoveEvent(KoPointerEvent *) override
    {
    }
    void mouseReleaseEvent(KoPointerEvent *) override
    {
    }
    void activate(const QSet<KoShape *> &shapes) override
    {
        ++activations;
        KoToolBase::activate(shapes);
    }
    int activations = 0;
};

class TestFactory : public KoToolFactoryBase
{
public:
    explicit TestFactory(const QString &id)
        : KoToolFactoryBase(id)
    {
        setSection(ToolBoxSection::Main);
        setPriority(id == brushId ? 0 : 1);
        setActivationShapeId("flake/always");
    }
    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new TestTool(canvas);
    }
};

struct CanvasFixture {
    ~CanvasFixture()
    {
        // Unregister while the controller's canvas() override is still alive.
        KoToolManager::instance()->removeCanvasController(&controller);
    }
    TestDocument document;
    TestCanvas canvas{&document};
    KisKActionCollection actions{static_cast<QObject *>(nullptr)};
    // Destroy the controller/tools before their canvas and action collection.
    TestController controller{&canvas, &actions};
};
} // namespace

class TestKoToolManager : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void testLookupUsesRequestedCanvas();
    void testDeviceSurvivesCanvasRoundTrip_data();
    void testDeviceSurvivesCanvasRoundTrip();
    void testNewDeviceUsesDestinationTool();
    void testExistingDeviceStatesStayIndependent();
    void testMouseOnlyRestoration();

private:
    void focus(CanvasFixture &fixture);
    KoToolManager *manager = KoToolManager::instance();
    QList<KoToolFactoryBase *> originalFactories;
    std::unique_ptr<CanvasFixture> a;
    std::unique_ptr<CanvasFixture> b;
};

void TestKoToolManager::initTestCase()
{
    // Exercise the real manager with inert tools; application tools require a KisCanvas2.
    KoShapeRegistry::instance();
    auto *registry = KoToolRegistry::instance();
    for (const QString &id : registry->keys()) {
        originalFactories.append(registry->value(id));
        registry->remove(id);
    }
    registry->add(new TestFactory(brushId));
    registry->add(new TestFactory(transformId));
}

void TestKoToolManager::cleanupTestCase()
{
    for (auto *factory : originalFactories) {
        KoToolRegistry::instance()->add(factory);
    }
}

void TestKoToolManager::init()
{
    a = std::make_unique<CanvasFixture>();
    b = std::make_unique<CanvasFixture>();
    manager->addController(&a->controller);
    manager->addController(&b->controller);
}

void TestKoToolManager::cleanup()
{
    b.reset();
    a.reset();
    QVERIFY(!manager->activeCanvasController());
}

void TestKoToolManager::focus(CanvasFixture &fixture)
{
    // The QApplication focusChanged connection reaches this same private slot.
    QVERIFY(QMetaObject::invokeMethod(manager,
                                      "movedFocus",
                                      Qt::DirectConnection,
                                      Q_ARG(QWidget *, nullptr),
                                      Q_ARG(QWidget *, &fixture.canvas.widget)));
    QCOMPARE(manager->activeCanvasController(), &fixture.controller);
}

void TestKoToolManager::testLookupUsesRequestedCanvas()
{
    auto *bTool = manager->toolById(&b->canvas, transformId);
    QVERIFY(bTool);
    focus(*a);
    auto *aTool = manager->toolById(&a->canvas, transformId);
    QVERIFY(aTool);
    QVERIFY(aTool != bTool);
    QSignalSpy changed(manager, &KoToolManager::changedTool);
    QCOMPARE(manager->toolById(&b->canvas, transformId), bTool);
    QCOMPARE(manager->toolById(&b->canvas, transformId)->canvas(), &b->canvas);
    QVERIFY(!manager->toolById(&b->canvas, QStringLiteral("missing")));
    TestDocument document;
    TestCanvas unregistered(&document);
    QVERIFY(!manager->toolById(&unregistered, transformId));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(manager->activeCanvasController(), &a->controller);
    focus(*b);
    QCOMPARE(manager->toolById(&a->canvas, transformId), aTool);
}

void TestKoToolManager::testDeviceSurvivesCanvasRoundTrip_data()
{
    QTest::addColumn<KoInputDevice>("device");
    QTest::newRow("pen") << KoInputDevice(KoInputDevice::InputDevice::Stylus, KoInputDevice::Pointer::Pen, 123);
    QTest::newRow("eraser") << KoInputDevice(KoInputDevice::InputDevice::Stylus, KoInputDevice::Pointer::Eraser, 123);
}

void TestKoToolManager::testDeviceSurvivesCanvasRoundTrip()
{
    QFETCH(KoInputDevice, device);
    manager->switchToolRequested(transformId);
    auto *transform = static_cast<TestTool *>(manager->toolById(&b->canvas, transformId));
    manager->switchInputDeviceRequested(device);
    manager->switchToolRequested(brushId);
    const int activations = transform->activations;
    QSignalSpy deviceChanges(manager, &KoToolManager::inputDeviceChanged);

    focus(*a); // A has no state for this device yet.
    QCOMPARE(manager->currentInputDevice(), device);
    QCOMPARE(manager->activeToolId(), brushId);
    manager->switchInputDeviceRequested(KoInputDevice::mouse()); // Mouse used to click tabs/settings.
    QCOMPARE(manager->currentInputDevice(), device);
    focus(*b);
    QCOMPARE(manager->currentInputDevice(), device);
    QCOMPARE(manager->activeToolId(), brushId);
    QCOMPARE(transform->activations, activations); // No transient transform stroke either.
    QCOMPARE(deviceChanges.count(), 0);
    focus(*a);
    focus(*b);
    QCOMPARE(manager->activeToolId(), brushId);
}

void TestKoToolManager::testNewDeviceUsesDestinationTool()
{
    focus(*a);
    manager->switchToolRequested(transformId);
    auto *transform = static_cast<TestTool *>(manager->toolById(&a->canvas, transformId));
    const int activations = transform->activations;
    focus(*b);
    const KoInputDevice pen = KoInputDevice::stylus();
    manager->switchInputDeviceRequested(pen);
    focus(*a);
    QCOMPARE(manager->currentInputDevice(), pen);
    QCOMPARE(manager->activeToolId(), transformId);
    QCOMPARE(transform->activations, activations + 1);
    focus(*a); // Repeated focus must not reactivate a transform.
    QCOMPARE(transform->activations, activations + 1);
}

void TestKoToolManager::testExistingDeviceStatesStayIndependent()
{
    const KoInputDevice pen = KoInputDevice::stylus();
    const KoInputDevice eraser = KoInputDevice::eraser();
    manager->switchInputDeviceRequested(pen);
    manager->switchToolRequested(brushId);
    manager->switchInputDeviceRequested(eraser);
    manager->switchToolRequested(transformId);
    focus(*a);
    QCOMPARE(manager->currentInputDevice(), eraser);
    manager->switchInputDeviceRequested(pen);
    focus(*b);
    QCOMPARE(manager->activeToolId(), brushId);
    manager->switchInputDeviceRequested(eraser);
    QCOMPARE(manager->activeToolId(), transformId);
    manager->switchInputDeviceRequested(pen);
    QCOMPARE(manager->activeToolId(), brushId);
}

void TestKoToolManager::testMouseOnlyRestoration()
{
    manager->switchToolRequested(transformId);
    focus(*a);
    QCOMPARE(manager->activeToolId(), brushId);
    focus(*b);
    QCOMPARE(manager->activeToolId(), transformId);
    QVERIFY(manager->currentInputDevice().isMouse());
}

KISTEST_MAIN(TestKoToolManager)
#include "TestKoToolManager.moc"
