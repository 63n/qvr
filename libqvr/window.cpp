/*
 * Copyright (C) 2016 Computer Graphics Group, University of Siegen
 * Written by Martin Lambers <martin.lambers@uni-siegen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <cstring>
#include <cmath>

#include <QThread>
#include <QMutex>
#include <QOpenGLShaderProgram>
#include <QOpenGLContext>
#include <QQuaternion>
#include <QCoreApplication>
#include <QApplication>
#include <QDesktopWidget>
#include <QScreen>
#include <QKeyEvent>
#include <QLibrary>

#include "window.hpp"
#include "manager.hpp"
#include "logging.hpp"
#include "observer.hpp"
#include "internalglobals.hpp"

#ifdef HAVE_OCULUS
# include <OVR_CAPI_GL.h>
#endif


class QVRWindowThread : public QThread
{
private:
    QVRWindow* _window;

public:
    bool exitWanted;
    QMutex renderingMutex;
    bool renderingFinished;
    QMutex swapbuffersMutex;
    bool swapbuffersFinished;

#if defined(HAVE_OCULUS) && (OVR_PRODUCT_VERSION < 1)
    ovrGLTexture oculusEyeTextures[2];
#endif

    QVRWindowThread(QVRWindow* window);

protected:
    void run() override;
};

QVRWindowThread::QVRWindowThread(QVRWindow* window) :
    _window(window), exitWanted(false)
{
}

void QVRWindowThread::run()
{
    for (;;) {
        _window->winContext()->makeCurrent(_window);
        // Start rendering
        renderingMutex.lock();
        if (!exitWanted) {
            _window->renderOutput();
        }
        renderingMutex.unlock();
        renderingFinished = true;
        if (exitWanted)
            break;
        // Swap buffers
        swapbuffersMutex.lock();
        if (!exitWanted) {
            if (_window->config().outputMode() == QVR_Output_Stereo_Oculus) {
#ifdef HAVE_OCULUS
# if (OVR_PRODUCT_VERSION >= 1)
                ovr_CommitTextureSwapChain(QVROculus, QVROculusTextureSwapChainL);
                ovr_CommitTextureSwapChain(QVROculus, QVROculusTextureSwapChainR);
                QVROculusLayer.RenderPose[0] = QVROculusRenderPoses[0];
                QVROculusLayer.RenderPose[1] = QVROculusRenderPoses[1];
                ovrLayerHeader* layers = &QVROculusLayer.Header;
                ovr_SubmitFrame(QVROculus, QVROculusFrameIndex, NULL, &layers, 1);
# else
                ovrHmd_EndFrame(QVROculus, QVROculusRenderPoses,
                        reinterpret_cast<ovrTexture*>(oculusEyeTextures));
# endif
#endif
            } else if (_window->config().outputMode() == QVR_Output_Stereo_OpenVR) {
#ifdef HAVE_OPENVR
                QVRUpdateOpenVR();
#endif
            } else {
                _window->winContext()->swapBuffers(_window);
            }
        }
        swapbuffersMutex.unlock();
        swapbuffersFinished = true;
        if (exitWanted)
            break;
    }
    _window->winContext()->doneCurrent();
    _window->winContext()->moveToThread(QCoreApplication::instance()->thread());
}

QVRWindow::QVRWindow(QOpenGLContext* masterContext,
        QVRObserver* observer,
        int processIndex, int windowIndex) :
    QWindow(),
    QOpenGLFunctions_3_3_Core(),
    _isValid(true),
    _thread(NULL),
    _observer(observer),
    _processIndex(processIndex),
    _windowIndex(windowIndex),
    _textures { 0, 0 },
    _outputQuadVao(0),
    _outputPrg(NULL),
    _renderContext()
{
    setSurfaceType(OpenGLSurface);
    create();
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    bool wantDoubleBuffer = true;
    // We do not want double buffering in the following cases:
    // - master context: never renders to screen
    // - Oculus or OpenVR control / mirror window: double-buffering this
    //   would cause libqvr to sync to the window's swap rate instead of
    //   the faster HMD swap rate
    if (isMaster()
            || config().outputMode() == QVR_Output_Stereo_Oculus
            || config().outputMode() == QVR_Output_Stereo_OpenVR) {
        wantDoubleBuffer = false;
    }
    format.setSwapBehavior(wantDoubleBuffer ? QSurfaceFormat::DoubleBuffer : QSurfaceFormat::SingleBuffer);
    if (isMaster()) {
        _winContext = masterContext;
    } else {
        _winContext = new QOpenGLContext;
        _winContext->setShareContext(masterContext);
    }
    setFormat(format);
    _winContext->setFormat(format);
    _winContext->create();
    if (!_winContext->isValid()) {
        QVR_FATAL("Cannot get a valid OpenGL context");
        _isValid = false;
        return;
    }
    if (!isMaster() && !QOpenGLContext::areSharing(_winContext, masterContext)) {
        QVR_FATAL("Cannot get a sharing OpenGL context");
        _isValid = false;
        return;
    }
    if (!initGL()) {
        _isValid = false;
        return;
    }

    // Disable the close button, since we cannot really properly handle it.
    setFlags(flags() | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    // Set an icon
    setIcon(QIcon(":/libqvr/cg-logo.png"));

    if (!isMaster()) {
        QVR_DEBUG("    creating window %s...", qPrintable(config().id()));
        if (QVRManager::config().processConfigs().size() > 0) {
            setTitle(processConfig().id() + " - " + config().id());
        } else {
            setTitle(config().id());
        }
        setMinimumSize(QSize(64, 64));
        if (config().outputMode() == QVR_Output_OSVR) {
#ifdef HAVE_OSVR
            if (config().initialDisplayScreen() >= 0) {
                QVR_DEBUG("      screen: %d", config().initialDisplayScreen());
                setScreen(QApplication::screens().at(config().initialDisplayScreen()));
            }
            OSVR_DisplayDimension w, h;
            osvrClientGetDisplayDimensions(QVROsvrDisplayConfig, 0, &w, &h);
            QRect geom = QApplication::desktop()->screenGeometry(config().initialDisplayScreen());
            geom.setWidth(w);
            geom.setHeight(h);
            QVR_DEBUG("      geometry: %d %d %dx%d", geom.x(), geom.y(), geom.width(), geom.height());
            setGeometry(geom);
            _winContext->makeCurrent(this);
            OSVR_OpenResultsOpenGL r;
            osvrRenderManagerOpenDisplayOpenGL(QVROsvrRenderManagerOpenGL, &r);
            if (r.status == OSVR_OPEN_STATUS_FAILURE)
                QVR_FATAL("OSVR: render manager failed to open display");
            _winContext->doneCurrent();
            setCursor(Qt::BlankCursor);
            show();
#endif
#if defined(HAVE_OCULUS) && (OVR_PRODUCT_VERSION < 1)
        } else if (config().outputMode() == QVR_Output_Stereo_Oculus) {
            unsigned int distortionCaps =
                ovrDistortionCap_TimeWarp
                | ovrDistortionCap_Vignette
                | ovrDistortionCap_NoRestore
                | ovrDistortionCap_Overdrive
                | ovrDistortionCap_HqDistortion
                | ovrDistortionCap_LinuxDevFullscreen;
            ovrGLConfig glconf;
            std::memset(&glconf, 0, sizeof(glconf));
            glconf.OGL.Header.API = ovrRenderAPI_OpenGL;
            glconf.OGL.Header.BackBufferSize.w = QVROculus->Resolution.w;
            glconf.OGL.Header.BackBufferSize.h = QVROculus->Resolution.h;
            glconf.OGL.Header.Multisample = 1;
            _winContext->makeCurrent(this);
            ovrHmd_ConfigureRendering(QVROculus,
                    reinterpret_cast<ovrRenderAPIConfig*>(&glconf),
                    distortionCaps, QVROculus->DefaultEyeFov,
                    QVROculusEyeRenderDesc);
            ovrHmd_DismissHSWDisplay(QVROculus);
            _winContext->doneCurrent();
            int oculusScreen = -1;
            for (int i = QApplication::desktop()->screenCount() - 1; i >= 0; i--) {
                if (QApplication::desktop()->screenGeometry(i).width() == QVROculus->Resolution.w
                        && QApplication::desktop()->screenGeometry(i).height() == QVROculus->Resolution.h) {
                    oculusScreen = i;
                    break;
                }
            }
            QVR_DEBUG("      screen: %d", oculusScreen);
            if (oculusScreen >= 0)
                setScreen(QApplication::screens().at(oculusScreen));
            QRect geom = QApplication::desktop()->screenGeometry(oculusScreen);
            QVR_DEBUG("      geometry: %d %d %dx%d", geom.x(), geom.y(), geom.width(), geom.height());
            setGeometry(geom);
            setCursor(Qt::BlankCursor);
            show(); // Apparently this must be called before showFullScreen()
            showFullScreen();
#endif
        } else {
            if (config().initialDisplayScreen() >= 0) {
                QVR_DEBUG("      screen: %d", config().initialDisplayScreen());
                setScreen(QApplication::screens().at(config().initialDisplayScreen()));
            }
            if (config().initialFullscreen()) {
                QRect screenGeom = QApplication::desktop()->screenGeometry(config().initialDisplayScreen());
                QVR_DEBUG("      fullscreen geometry: %d %d %dx%d", screenGeom.x(), screenGeom.y(), screenGeom.width(), screenGeom.height());
                setGeometry(screenGeom);
                setCursor(Qt::BlankCursor);
                show(); // Apparently this must be called before showFullScreen()
                showFullScreen();
            } else {
                if (config().initialPosition().x() >= 0 && config().initialPosition().y() >= 0) {
                    QVR_DEBUG("      position %d,%d size %dx%d",
                            config().initialPosition().x(), config().initialPosition().y(),
                            config().initialSize().width(), config().initialSize().height());
                    QRect screenGeom = QApplication::desktop()->screenGeometry(config().initialDisplayScreen());
                    setGeometry(
                            config().initialPosition().x() + screenGeom.x(),
                            config().initialPosition().y() + screenGeom.y(),
                            config().initialSize().width(), config().initialSize().height());
                } else {
                    QVR_DEBUG("      size %dx%d", config().initialSize().width(), config().initialSize().height());
                    resize(config().initialSize());
                }
                show();
            }
        }
        raise();

        _thread = new QVRWindowThread(this);
        _winContext->moveToThread(_thread);
        _thread->renderingMutex.lock();
        _thread->swapbuffersMutex.lock();
        _thread->start();

        _renderContext.setProcessIndex(this->processIndex());
        _renderContext.setWindowIndex(index());

        QVR_DEBUG("    ... done");
    }
}

QVRWindow::~QVRWindow()
{
    if (_thread) {
        exitGL();
        winContext()->deleteLater();
    }
}

void QVRWindow::renderToScreen()
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);

    _thread->renderingFinished = false;
    _thread->renderingMutex.unlock();
    while (!_thread->renderingFinished)
        QThread::yieldCurrentThread();
    _thread->renderingMutex.lock();
}

void QVRWindow::asyncSwapBuffers()
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);

    _thread->swapbuffersFinished = false;
    _thread->swapbuffersMutex.unlock();
}

void QVRWindow::waitForSwapBuffers()
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);

    while (!_thread->swapbuffersFinished)
        QThread::usleep(1);
    _thread->swapbuffersMutex.lock();
}

bool QVRWindow::isMaster() const
{
    return !_observer;
}

int QVRWindow::index() const
{
    return _windowIndex;
}

const QString& QVRWindow::id() const
{
    return config().id();
}

const QVRWindowConfig& QVRWindow::config() const
{
    return processConfig().windowConfigs().at(index());
}

int QVRWindow::processIndex() const
{
    return _processIndex;
}

const QString& QVRWindow::processId() const
{
    return processConfig().id();
}

const QVRProcessConfig& QVRWindow::processConfig() const
{
    return QVRManager::config().processConfigs().at(processIndex());
}

int QVRWindow::observerIndex() const
{
    return _observer->index();
}

const QString& QVRWindow::observerId() const
{
    return _observer->id();
}

const QVRObserverConfig& QVRWindow::observerConfig() const
{
    return _observer->config();
}

bool QVRWindow::initGL()
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    _winContext->makeCurrent(this);
    if (!initializeOpenGLFunctions()) {
        QVR_FATAL("Cannot initialize OpenGL functions");
        return false;
    }

    if (!isMaster()) {
        if (config().outputPlugin().isEmpty()) {
            // Initialize our own output code
            static QVector3D quadPositions[] = {
                QVector3D(-1.0f, -1.0f, 0.0f), QVector3D(+1.0f, -1.0f, 0.0f),
                QVector3D(+1.0f, +1.0f, 0.0f), QVector3D(-1.0f, +1.0f, 0.0f)
            };
            static QVector2D quadTexcoords[] = {
                QVector2D(0.0f, 0.0f), QVector2D(1.0f, 0.0f),
                QVector2D(1.0f, 1.0f), QVector2D(0.0f, 1.0f)
            };
            static GLuint quadIndices[] = {
                0, 1, 3, 1, 2, 3
            };
            glGenVertexArrays(1, &_outputQuadVao);
            glBindVertexArray(_outputQuadVao);
            GLuint positionBuffer;
            glGenBuffers(1, &positionBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, positionBuffer);
            glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(QVector3D), quadPositions, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(0);
            GLuint texcoordBuffer;
            glGenBuffers(1, &texcoordBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, texcoordBuffer);
            glBufferData(GL_ARRAY_BUFFER, 4 * sizeof(QVector2D), quadTexcoords, GL_STATIC_DRAW);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glEnableVertexAttribArray(1);
            GLuint indexBuffer;
            glGenBuffers(1, &indexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6 * sizeof(GLuint), quadIndices, GL_STATIC_DRAW);
            glBindVertexArray(0);
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                QVR_FATAL("OpenGL error 0x%04X\n", e);
                return false;
            }

            _outputPrg = new QOpenGLShaderProgram(this);
            if (!_outputPrg->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/libqvr/output-vs.glsl")) {
                QVR_FATAL("Cannot add output vertex shader");
                return false;
            }
            if (!_outputPrg->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/libqvr/output-fs.glsl")) {
                QVR_FATAL("Cannot add output fragment shader");
                return false;
            }
            if (!_outputPrg->link()) {
                QVR_FATAL("Cannot link output program");
                return false;
            }
        } else {
            // Initialize output plugin
            QStringList pluginSpec = config().outputPlugin().split(' ', QString::SkipEmptyParts);
            QString pluginPath = pluginSpec.at(0);
            QStringList pluginArgs = pluginSpec.mid(1);
            QLibrary plugin(pluginPath);
            if (!plugin.load()) {
                QVR_FATAL("Cannot load output plugin %s", qPrintable(pluginPath));
                return false;
            }
            _outputPluginInitFunc = reinterpret_cast<bool (*)(QVRWindow*, const QStringList&)>
                (plugin.resolve("QVROutputPluginInit"));
            _outputPluginExitFunc = reinterpret_cast<void (*)(QVRWindow*)>
                (plugin.resolve("QVROutputPluginExit"));
            _outputPluginFunc = reinterpret_cast<void (*)(QVRWindow*, const QVRRenderContext&,
                    unsigned int, unsigned int)>
                (plugin.resolve("QVROutputPlugin"));
            if (!_outputPluginInitFunc || !_outputPluginExitFunc || !_outputPluginFunc) {
                QVR_FATAL("Cannot resolve output plugin functions from plugin %s", qPrintable(pluginPath));
                return false;
            }
            if (!_outputPluginInitFunc(this, pluginArgs)) {
                QVR_FATAL("Cannot initialize output plugin %s", qPrintable(pluginPath));
                return false;
            }
        }
    }

    _winContext->doneCurrent();
    return true;
}

void QVRWindow::exitGL()
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);

    if (!isMaster() && _thread) {
        if (_thread->isRunning()) {
            _thread->exitWanted = 1;
            _thread->renderingMutex.unlock();
            _thread->swapbuffersMutex.unlock();
            _thread->wait();
        }
        delete _thread;
        _thread = NULL;
        _winContext->makeCurrent(this);
        if (config().outputPlugin().isEmpty()) {
            glDeleteTextures(2, _textures);
            glDeleteVertexArrays(1, &_outputQuadVao);
            delete _outputPrg;
        } else {
            _outputPluginExitFunc(this);
        }
        _winContext->doneCurrent();
    }
}

void QVRWindow::screenWall(QVector3D& cornerBottomLeft, QVector3D& cornerBottomRight, QVector3D& cornerTopLeft)
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);
    Q_ASSERT(config().outputMode() != QVR_Output_Stereo_Oculus);
    Q_ASSERT(config().outputMode() != QVR_Output_OSVR);

    if (config().screenIsGivenByCenter()) {
        // Get geometry (in meter) of the screen
        QDesktopWidget* desktop = QApplication::desktop();
        QRect monitorGeom = desktop->screenGeometry(config().initialDisplayScreen());
        float monitorWidth = monitorGeom.width() / desktop->logicalDpiX() * 0.0254f;
        float monitorHeight = monitorGeom.height() / desktop->logicalDpiY() * 0.0254f;
        cornerBottomLeft.setX(-monitorWidth / 2.0f);
        cornerBottomLeft.setY(-monitorHeight / 2.0f);
        cornerBottomLeft.setZ(0.0f);
        cornerBottomRight.setX(+monitorWidth / 2.0f);
        cornerBottomRight.setY(-monitorHeight / 2.0f);
        cornerBottomRight.setZ(0.0f);
        cornerTopLeft.setX(-monitorWidth / 2.0f);
        cornerTopLeft.setY(+monitorHeight / 2.0f);
        cornerTopLeft.setZ(0.0f);
        QVector3D cornerTopRight = cornerBottomRight + (cornerTopLeft - cornerBottomLeft);
        // Apply the window geometry (subset of screen)
        QRect windowGeom = geometry();
        float windowX = static_cast<float>(windowGeom.x() - monitorGeom.x()) / monitorGeom.width();
        float windowY = 1.0f - static_cast<float>(windowGeom.y() + windowGeom.height() - monitorGeom.y()) / monitorGeom.height();
        float windowW = static_cast<float>(windowGeom.width()) / monitorGeom.width();
        float windowH = static_cast<float>(windowGeom.height()) / monitorGeom.height();
        QVector3D l0 = (1.0f - windowX) * cornerBottomLeft + windowX * cornerBottomRight;
        QVector3D l1 = (1.0f - windowX) * cornerTopLeft  + windowX * cornerTopRight;
        QVector3D bl = (1.0f - windowY) * l0 + windowY * l1;
        QVector3D tl = (1.0f - windowY - windowH) * l0 + (windowY + windowH) * l1;
        QVector3D r0 = (1.0f - windowX - windowW) * cornerBottomLeft + (windowX + windowW) * cornerBottomRight;
        QVector3D r1 = (1.0f - windowX - windowW) * cornerTopLeft + (windowX + windowW) * cornerTopRight;
        QVector3D br = (1.0f - windowY) * r0 + windowY * r1;
        cornerBottomLeft = bl;
        cornerBottomRight = br;
        cornerTopLeft = tl;
        // Translate according to screen center position
        cornerBottomLeft += config().screenCenter();
        cornerBottomRight += config().screenCenter();
        cornerTopLeft += config().screenCenter();
    } else {
        cornerBottomLeft = config().screenCornerBottomLeft();
        cornerBottomRight = config().screenCornerBottomRight();
        cornerTopLeft = config().screenCornerTopLeft();
    }
    if (config().screenIsFixedToObserver()) {
        QMatrix4x4 o = _observer->trackingMatrix();
        cornerBottomLeft = o * cornerBottomLeft;
        cornerBottomRight = o * cornerBottomRight;
        cornerTopLeft = o * cornerTopLeft;
    }
}

#ifdef HAVE_OSVR
static OSVR_RenderInfoOpenGL QVROsvrRenderInfoOpenGL[2];
static OSVR_RenderBufferOpenGL QVROsvrRenderBufferOpenGL[2];
#endif

const QVRRenderContext& QVRWindow::computeRenderContext(float n, float f, unsigned int textures[2])
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    Q_ASSERT(QOpenGLContext::currentContext() != _winContext);

    /* Compute the render context */

    _renderContext.setWindowGeometry(geometry());
    _renderContext.setScreenGeometry(screen()->geometry());
    _renderContext.setNavigation(_observer->navigationPosition(), _observer->navigationOrientation());
    _renderContext.setOutputConf(config().outputMode());
    QVector3D wallBl, wallBr, wallTl;
    if (config().outputMode() != QVR_Output_Stereo_Oculus && config().outputMode() != QVR_Output_OSVR)
        screenWall(wallBl, wallBr, wallTl);
    _renderContext.setScreenWall(wallBl, wallBr, wallTl);
    for (int i = 0; i < _renderContext.viewPasses(); i++) {
        QVREye eye = _renderContext.eye(i);
        _renderContext.setTracking(i, _observer->trackingPosition(eye), _observer->trackingOrientation(eye));
        QVector3D viewPos;
        QQuaternion viewRot;
        if (config().outputMode() == QVR_Output_Stereo_Oculus) {
#ifdef HAVE_OCULUS
            const ovrFovPort& fov = QVROculusEyeRenderDesc[i].Fov;
            _renderContext.setFrustum(i, QVRFrustum(
                        -fov.LeftTan * n,
                        fov.RightTan * n,
                        -fov.DownTan * n,
                        fov.UpTan * n,
                        n, f));
#endif
            viewPos = _renderContext.trackingPosition(i);
            viewRot = _renderContext.trackingOrientation(i);
        } else if (config().outputMode() == QVR_Output_Stereo_OpenVR) {
            float l = 0.0f, r = 0.0f, b = 0.0f, t = 0.0f;
#ifdef HAVE_OPENVR
            QVROpenVRSystem->GetProjectionRaw(
                    eye == QVR_Eye_Left ? vr::Eye_Left : vr::Eye_Right,
                    &l, &r, &t, &b);
#endif
            QVRFrustum frustum(l, r, t, b, 1.0f, f);
            frustum.adjustNearPlane(n);
            _renderContext.setFrustum(i, frustum);
            viewPos = _renderContext.trackingPosition(i);
            viewRot = _renderContext.trackingOrientation(i);
        } else if (config().outputMode() == QVR_Output_OSVR) {
            double l = 0.0, r = 0.0, b = 0.0, t = 0.0;
#ifdef HAVE_OSVR
            osvrClientGetViewerEyeSurfaceProjectionClippingPlanes(
                    QVROsvrDisplayConfig, 0, i, 0, &l, &r, &b, &t);
#endif
            QVRFrustum frustum(l, r, b, t, 1.0f, f);
            frustum.adjustNearPlane(n);
            _renderContext.setFrustum(i, frustum);
            viewPos = _renderContext.trackingPosition(i);
            viewRot = _renderContext.trackingOrientation(i);
        } else {
            // Determine the eye position
            QVector3D eyePosition = _observer->trackingPosition(eye);
            // Get the geometry of the screen area relative to the eye
            QVector3D bl = wallBl - eyePosition;
            QVector3D br = wallBr - eyePosition;
            QVector3D tl = wallTl - eyePosition;
            // Compute the HNF of the screen plane
            QVector3D planeRight = (br - bl).normalized();
            QVector3D planeUp = (tl - bl).normalized();
            QVector3D planeNormal = QVector3D::crossProduct(planeUp, planeRight);
            float planeDistance = QVector3D::dotProduct(planeNormal, bl);
            // Compute the frustum
            float width = (br - bl).length();
            float height = (tl - bl).length();
            float l = -QVector3D::dotProduct(-bl, planeRight);
            float r = width + l;
            float b = -QVector3D::dotProduct(-bl, planeUp);
            float t = height + b;
            float q = n / planeDistance;
            _renderContext.setFrustum(i, QVRFrustum(l * q, r * q, b * q, t * q, n, f));
            // Compute the view matrix
            QVector3D eyeProjection = -QVector3D::dotProduct(-bl, planeNormal) * planeNormal;
            viewPos = eyePosition;
            viewRot = QQuaternion::fromDirection(-eyeProjection, planeUp);
        }
        QMatrix4x4 viewMatrix;
        if (config().screenIsFixedToObserver()) {
            // XXX why is this special case necessary?? the code below should always work!
            viewMatrix.rotate(viewRot.inverted());
            viewMatrix.rotate(_renderContext.navigationOrientation().inverted());
            viewMatrix.translate(-viewPos);
            viewMatrix.translate(-_renderContext.navigationPosition());
        } else {
            viewMatrix.rotate(viewRot.inverted());
            viewMatrix.translate(-viewPos);
            viewMatrix.rotate(_renderContext.navigationOrientation().inverted());
            viewMatrix.translate(-_renderContext.navigationPosition());
        }
        _renderContext.setViewMatrix(i, viewMatrix);
    }

    /* Get the textures that the application needs to render into */

    GLint textureBinding2dBak;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding2dBak);

#if defined(HAVE_OCULUS) && (OVR_PRODUCT_VERSION >= 1)
    if (config().outputMode() == QVR_Output_Stereo_Oculus && _textures[0] == 0) {
        ovrHmdDesc hmdDesc = ovr_GetHmdDesc(QVROculus);
        ovrTextureSwapChainDesc tscDesc = {};
        tscDesc.Type = ovrTexture_2D;
        tscDesc.ArraySize = 1;
        tscDesc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
        tscDesc.MipLevels = 1;
        tscDesc.SampleCount = 1;
        tscDesc.StaticImage = ovrFalse;
        ovrSizei texSizeL = ovr_GetFovTextureSize(QVROculus, ovrEye_Left, hmdDesc.DefaultEyeFov[0], 1.0f);
        tscDesc.Width = texSizeL.w * config().renderResolutionFactor();
        tscDesc.Height = texSizeL.h * config().renderResolutionFactor();
        ovr_CreateTextureSwapChainGL(QVROculus, &tscDesc, &QVROculusTextureSwapChainL);
        ovrSizei texSizeR = ovr_GetFovTextureSize(QVROculus, ovrEye_Right, hmdDesc.DefaultEyeFov[1], 1.0f);
        tscDesc.Width = texSizeR.w * config().renderResolutionFactor();
        tscDesc.Height = texSizeR.h * config().renderResolutionFactor();
        ovr_CreateTextureSwapChainGL(QVROculus, &tscDesc, &QVROculusTextureSwapChainR);
        ovr_GetTextureSwapChainBufferGL(QVROculus, QVROculusTextureSwapChainL, 0, &(_textures[0]));
        ovr_GetTextureSwapChainBufferGL(QVROculus, QVROculusTextureSwapChainR, 0, &(_textures[1]));
        QVROculusLayer.Header.Type = ovrLayerType_EyeFov;
        QVROculusLayer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft;
        QVROculusLayer.ColorTexture[0] = QVROculusTextureSwapChainL;
        QVROculusLayer.ColorTexture[1] = QVROculusTextureSwapChainR;
        QVROculusLayer.Fov[0] = QVROculusEyeRenderDesc[0].Fov;
        QVROculusLayer.Fov[1] = QVROculusEyeRenderDesc[1].Fov;
        ovrRecti vpL;
        vpL.Pos.x = 0;
        vpL.Pos.y = 0;
        vpL.Size.w = texSizeL.w;
        vpL.Size.h = texSizeL.h;
        QVROculusLayer.Viewport[0] = vpL;
        ovrRecti vpR;
        vpR.Pos.x = 0;
        vpR.Pos.y = 0;
        vpR.Size.w = texSizeR.w;
        vpR.Size.h = texSizeR.h;
        QVROculusLayer.Viewport[1] = vpR;
    }
#endif
#ifdef HAVE_OSVR
    bool texturesNeedRegistering = false;
    OSVR_RenderInfoCollection osvrRenderInfoCollection;
    if (config().outputMode() == QVR_Output_OSVR && _textures[0] == 0) {
        OSVR_RenderParams osvrRenderParams;
        osvrRenderManagerGetDefaultRenderParams(&osvrRenderParams);
        osvrRenderManagerGetRenderInfoCollection(QVROsvrRenderManager, osvrRenderParams, &osvrRenderInfoCollection);
        OSVR_RenderInfoCount osvrRenderInfoCount;
        osvrRenderManagerGetNumRenderInfoInCollection(osvrRenderInfoCollection, &osvrRenderInfoCount);
        Q_ASSERT(osvrRenderInfoCount == static_cast<unsigned int>(_renderContext.viewPasses()));
    }
#endif
    for (int i = 0; i < _renderContext.viewPasses(); i++) {
        int tw = -1, th = -1;
        if (_textures[i] == 0) {
            glGenTextures(1, &(_textures[i]));
            glBindTexture(GL_TEXTURE_2D, _textures[i]);
            bool wantBilinearInterpolation = true;
            if (std::abs(config().renderResolutionFactor() - 1.0f) <= 0.0f
                    && (config().outputMode() == QVR_Output_Center
                        || config().outputMode() == QVR_Output_Left
                        || config().outputMode() == QVR_Output_Right
                        || config().outputMode() == QVR_Output_Stereo_GL
                        || config().outputMode() == QVR_Output_Stereo_Red_Cyan
                        || config().outputMode() == QVR_Output_Stereo_Green_Magenta
                        || config().outputMode() == QVR_Output_Stereo_Amber_Blue)) {
                wantBilinearInterpolation = false;
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, wantBilinearInterpolation ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, wantBilinearInterpolation ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#ifdef HAVE_OSVR
            texturesNeedRegistering = true;
#endif
        } else {
            glBindTexture(GL_TEXTURE_2D, _textures[i]);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        }
        int w = 0, h = 0;
        if (config().outputMode() == QVR_Output_Stereo_Oculus) {
#ifdef HAVE_OCULUS
# if (OVR_PRODUCT_VERSION >= 1)
            // we already created the textures before this loop, make sure that we don't do
            // anything inside this loop.
            w = tw;
            h = th;
# else
            ovrSizei tex_size = ovrHmd_GetFovTextureSize(QVROculus,
                    i == 0 ? ovrEye_Left : ovrEye_Right,
                    QVROculus->DefaultEyeFov[i], 1.0f);
            w = tex_size.w * config().renderResolutionFactor();
            h = tex_size.h * config().renderResolutionFactor();
            _thread->oculusEyeTextures[i].OGL.Header.API = ovrRenderAPI_OpenGL;
            _thread->oculusEyeTextures[i].OGL.Header.TextureSize.w = w;
            _thread->oculusEyeTextures[i].OGL.Header.TextureSize.h = h;
            _thread->oculusEyeTextures[i].OGL.Header.RenderViewport.Pos.x = 0;
            _thread->oculusEyeTextures[i].OGL.Header.RenderViewport.Pos.y = 0;
            _thread->oculusEyeTextures[i].OGL.Header.RenderViewport.Size.w = w;
            _thread->oculusEyeTextures[i].OGL.Header.RenderViewport.Size.h = h;
            _thread->oculusEyeTextures[i].OGL.TexId = _textures[i];
# endif
#endif
        } else if (config().outputMode() == QVR_Output_Stereo_OpenVR) {
#ifdef HAVE_OPENVR
            uint32_t openVrW, openVrH;
            QVROpenVRSystem->GetRecommendedRenderTargetSize(&openVrW, &openVrH);
            w = openVrW * config().renderResolutionFactor();
            h = openVrH * config().renderResolutionFactor();
#endif
        } else if (config().outputMode() == QVR_Output_OSVR) {
#ifdef HAVE_OSVR
            OSVR_ViewportDimension vpl, vpb, vpw, vph;
            osvrClientGetRelativeViewportForViewerEyeSurface(
                    QVROsvrDisplayConfig, 0, i, 0, &vpl, &vpb, &vpw, &vph);
            w = vpw * config().renderResolutionFactor();
            h = vph * config().renderResolutionFactor();
            osvrRenderManagerGetRenderInfoFromCollectionOpenGL(
                    osvrRenderInfoCollection, i, &(QVROsvrRenderInfoOpenGL[i]));
            // TODO: use osvrRenderInfoOpenGL to determine the texture sizes?
            // The render manager might want us to use a different texture size
            // than plain OSVR because of distortion correction effects.
            // However, currently the viewport fields in osvrRenderInfoOpenGL
            // seem to contain random garbage (2016-10-14).
#endif
        } else {
            w = width() * config().renderResolutionFactor();
            h = height() * config().renderResolutionFactor();
        }
        if (tw != w || th != h) {
            bool wantSRGB = true;
            if (config().outputMode() == QVR_Output_Stereo_OpenVR) {
                // 2016-11-03: OpenVR cannot seem to handle SRGB textures; neither
                // ColorSpace_Linear nor ColorSpace_Gamma give correct rendering
                // results. So fall back to linear textures.
                wantSRGB = false;
            }
            glTexImage2D(GL_TEXTURE_2D, 0,
                    wantSRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                    w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
#ifdef HAVE_OSVR
            texturesNeedRegistering = true;
#endif
        }
    }
    if (_renderContext.viewPasses() == 1 && _textures[1] != 0) {
        glDeleteTextures(1, &(_textures[1]));
        _textures[1] = 0;
    }
#ifdef HAVE_OSVR
    if (config().outputMode() == QVR_Output_OSVR) {
        osvrRenderManagerReleaseRenderInfoCollection(osvrRenderInfoCollection);
        if (texturesNeedRegistering) {
            OSVR_RenderManagerRegisterBufferState s;
            osvrRenderManagerStartRegisterRenderBuffers(&s);
            QVROsvrRenderBufferOpenGL[1].colorBufferName = 0;
            QVROsvrRenderBufferOpenGL[1].depthStencilBufferName = 0;
            for (int i = 0; i < _renderContext.viewPasses(); i++) {
                QVROsvrRenderBufferOpenGL[i].colorBufferName = _textures[i];
                QVROsvrRenderBufferOpenGL[i].depthStencilBufferName = 0;
                osvrRenderManagerRegisterRenderBufferOpenGL(s, QVROsvrRenderBufferOpenGL[i]);
            }
            osvrRenderManagerFinishRegisterRenderBuffers(QVROsvrRenderManager, s, false);
        }
    }
#endif
    textures[0] = _textures[0];
    textures[1] = _textures[1];
#if defined(HAVE_OCULUS) && (OVR_PRODUCT_VERSION >= 1)
    if (config().outputMode() == QVR_Output_Stereo_Oculus) {
        ovr_GetTextureSwapChainBufferGL(QVROculus, QVROculusTextureSwapChainL, -1, &(textures[0]));
        ovr_GetTextureSwapChainBufferGL(QVROculus, QVROculusTextureSwapChainR, -1, &(textures[1]));
    }
#endif

    glBindTexture(GL_TEXTURE_2D, textureBinding2dBak);

    return _renderContext;
}

void QVRWindow::renderOutput()
{
    Q_ASSERT(!isMaster());
    Q_ASSERT(QThread::currentThread() == _thread);
    Q_ASSERT(QOpenGLContext::currentContext() == _winContext);

    if (!config().outputPlugin().isEmpty()) {
        _outputPluginFunc(this, _renderContext, _textures[0], _textures[1]);
    } else if (config().outputMode() == QVR_Output_OSVR) {
#ifdef HAVE_OSVR
        const OSVR_ViewportDescription osvrDefaultViewport = { .left = 0, .lower = 0, .width = 1, .height = 1 };
        OSVR_RenderParams osvrDefaultRenderParams;
        osvrRenderManagerGetDefaultRenderParams(&osvrDefaultRenderParams);
        OSVR_RenderManagerPresentState s;
        osvrRenderManagerStartPresentRenderBuffers(&s);
        osvrRenderManagerPresentRenderBufferOpenGL(s,
                QVROsvrRenderBufferOpenGL[0],
                QVROsvrRenderInfoOpenGL[0],
                osvrDefaultViewport);
        if (QVROsvrRenderBufferOpenGL[1].colorBufferName != 0) {
            osvrRenderManagerPresentRenderBufferOpenGL(s,
                    QVROsvrRenderBufferOpenGL[1],
                    QVROsvrRenderInfoOpenGL[1],
                    osvrDefaultViewport);
        }
        osvrRenderManagerFinishPresentRenderBuffers(QVROsvrRenderManager, s, osvrDefaultRenderParams, false);
#endif
    } else {
        glDisable(GL_DEPTH_TEST);
        glUseProgram(_outputPrg->programId());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, _textures[0]);
        glUniform1i(glGetUniformLocation(_outputPrg->programId(), "tex_l"), 0);
        glUniform1i(glGetUniformLocation(_outputPrg->programId(), "tex_r"), 0);
        glUniform1i(glGetUniformLocation(_outputPrg->programId(), "output_mode"), config().outputMode());
        glBindVertexArray(_outputQuadVao);
        if (_textures[1] != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, _textures[1]);
            glUniform1i(glGetUniformLocation(_outputPrg->programId(), "tex_r"), 1);
        }
        glViewport(0, 0, width(), height());
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        if (config().outputMode() == QVR_Output_Stereo_OpenVR) {
#ifdef HAVE_OPENVR
            vr::Texture_t l = { reinterpret_cast<void*>(_textures[0]), vr::API_OpenGL, vr::ColorSpace_Linear };
            vr::VRCompositor()->Submit(vr::Eye_Left, &l, NULL, vr::Submit_Default);
            vr::Texture_t r = { reinterpret_cast<void*>(_textures[1]), vr::API_OpenGL, vr::ColorSpace_Linear };
            vr::VRCompositor()->Submit(vr::Eye_Right, &r, NULL, vr::Submit_Default);
            glFlush(); // suggested by a comment in openvr.h
#endif
        }
    }
}

void QVRWindow::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::FullScreen)
            || (event->key() == Qt::Key_F
                && (event->modifiers() & Qt::ShiftModifier)
                && (event->modifiers() & Qt::ControlModifier))
            || (event->key() == Qt::Key_F11)) {
        if (windowState() == Qt::WindowFullScreen)
            showNormal();
        else
            showFullScreen();
    } else {
        QVRManager::enqueueKeyPressEvent(_renderContext, event);
    }
}

void QVRWindow::keyReleaseEvent(QKeyEvent* event)
{
    QVRManager::enqueueKeyReleaseEvent(_renderContext, event);
}

void QVRWindow::mouseMoveEvent(QMouseEvent* event)
{
    QVRManager::enqueueMouseMoveEvent(_renderContext, event);
}

void QVRWindow::mousePressEvent(QMouseEvent* event)
{
    QVRManager::enqueueMousePressEvent(_renderContext, event);
}

void QVRWindow::mouseReleaseEvent(QMouseEvent* event)
{
    QVRManager::enqueueMouseReleaseEvent(_renderContext, event);
}

void QVRWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    QVRManager::enqueueMouseDoubleClickEvent(_renderContext, event);
}

void QVRWindow::wheelEvent(QWheelEvent* event)
{
    QVRManager::enqueueWheelEvent(_renderContext, event);
}
