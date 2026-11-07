#include "OBSQTDisplay.hpp"

#include <utility/display-helpers.hpp>
#include <utility/SurfaceEventFilter.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QWindow>
#ifdef __HAIKU__
#include <QPainter>

#include <cstring>
#endif
#ifdef ENABLE_WAYLAND
#include <QApplication>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "moc_OBSQTDisplay.cpp"

static inline long long color_to_int(const QColor &color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

static inline QColor rgba_to_color(uint32_t rgba)
{
	return QColor::fromRgb(rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 24) & 0xFF);
}

static bool QTToGSWindow(QWindow *window, gs_window &gswindow)
{
	bool success = true;

#ifdef _WIN32
	gswindow.hwnd = (HWND)window->winId();
#elif __APPLE__
	gswindow.view = (id)window->winId();
#elif defined(__HAIKU__)
	/* Handled in OBSQTDisplay::CreateDisplay (present callback) */
	UNUSED_PARAMETER(window);
	UNUSED_PARAMETER(gswindow);
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = window->winId();
		gswindow.display = obs_get_nix_platform_display();
		break;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND: {
		gswindow.display = (void *)window->winId();
		success = gswindow.display != nullptr;
		break;
	}
#endif
	default:
		success = false;
		break;
	}
#endif
	return success;
}

#ifdef __HAIKU__
OBSQTDisplay::OBSQTDisplay(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags)
{
	/* On Haiku every native QWindow is a separate BWindow, so embedded displays stay alien widgets
	 * and paint the frames libobs hands back. Top-level displays (projectors) still get a QWindow so
	 * windowHandle() is valid for callers. */
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	if (isWindow())
		setAttribute(Qt::WA_NativeWindow);
}

void OBSQTDisplay::HaikuPresent(void *param, const uint8_t *bgra, uint32_t width, uint32_t height,
				uint32_t linesize)
{
	OBSQTDisplay *self = static_cast<OBSQTDisplay *>(param);

	{
		std::lock_guard<std::mutex> lock(self->frameMutex);

		if (self->frame.width() != (int)width || self->frame.height() != (int)height)
			self->frame = QImage((int)width, (int)height, QImage::Format_RGB32);

		/* BGRA bytes == QImage::Format_RGB32 on little-endian */
		const size_t row = (size_t)width * 4;
		for (uint32_t y = 0; y < height; y++)
			memcpy(self->frame.scanLine((int)y), bgra + (size_t)y * linesize, row);
	}

	/* Called on the graphics thread: coalesce repaints onto the UI thread. */
	if (!self->frameUpdatePending.exchange(true)) {
		QMetaObject::invokeMethod(
			self,
			[self]() {
				self->frameUpdatePending = false;
				self->update();
			},
			Qt::QueuedConnection);
	}
}

void OBSQTDisplay::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!display) {
		CreateDisplay();
	} else {
		QSize size = GetPixelSize(this);
		obs_display_resize(display, size.width(), size.height());
	}
}

void OBSQTDisplay::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	display = nullptr;
}
#else
OBSQTDisplay::OBSQTDisplay(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	auto windowVisible = [this](bool visible) {
		if (!visible) {
#if !defined(_WIN32) && !defined(__APPLE__)
			display = nullptr;
#endif
			return;
		}

		if (!display) {
			CreateDisplay();
		} else {
			QSize size = GetPixelSize(this);
			obs_display_resize(display, size.width(), size.height());
		}
	};

	auto screenChanged = [this](QScreen *) {
		CreateDisplay();

		QSize size = GetPixelSize(this);
		obs_display_resize(display, size.width(), size.height());
	};

	connect(windowHandle(), &QWindow::visibleChanged, this, windowVisible);
	connect(windowHandle(), &QWindow::screenChanged, this, screenChanged);

	windowHandle()->installEventFilter(new SurfaceEventFilter(this));
}
#endif

QColor OBSQTDisplay::GetDisplayBackgroundColor() const
{
	return rgba_to_color(backgroundColor);
}

void OBSQTDisplay::SetDisplayBackgroundColor(const QColor &color)
{
	uint32_t newBackgroundColor = (uint32_t)color_to_int(color);

	if (newBackgroundColor != backgroundColor) {
		backgroundColor = newBackgroundColor;
		UpdateDisplayBackgroundColor();
	}
}

void OBSQTDisplay::UpdateDisplayBackgroundColor()
{
	obs_display_set_background_color(display, backgroundColor);
}

void OBSQTDisplay::CreateDisplay()
{
	if (display) {
		return;
	}

	if (destroying) {
		return;
	}

#ifdef __HAIKU__
	if (!isVisible() || width() <= 0 || height() <= 0) {
		return;
	}
#else
	if (!windowHandle()->isExposed()) {
		return;
	}
#endif

	QSize size = GetPixelSize(this);

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef __HAIKU__
	info.window.present = HaikuPresent;
	info.window.param = this;
#else
	if (!QTToGSWindow(windowHandle(), info.window)) {
		return;
	}
#endif

	display = obs_display_create(&info, backgroundColor);

	emit DisplayCreated(this);
}

void OBSQTDisplay::paintEvent(QPaintEvent *event)
{
	CreateDisplay();

#ifdef __HAIKU__
	UNUSED_PARAMETER(event);

	QPainter painter(this);
	std::lock_guard<std::mutex> lock(frameMutex);

	if (frame.isNull()) {
		painter.fillRect(rect(), GetDisplayBackgroundColor());
		return;
	}

	frame.setDevicePixelRatio(devicePixelRatioF());
	painter.drawImage(QPoint(0, 0), frame);
#else
	QWidget::paintEvent(event);
#endif
}

void OBSQTDisplay::moveEvent(QMoveEvent *event)
{
	QWidget::moveEvent(event);

	OnMove();
}

bool OBSQTDisplay::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_DISPLAYCHANGE:
		OnDisplayChange();
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void OBSQTDisplay::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	CreateDisplay();

	if (isVisible() && display) {
		QSize size = GetPixelSize(this);
		obs_display_resize(display, size.width(), size.height());
	}

	emit DisplayResized();
}

QPaintEngine *OBSQTDisplay::paintEngine() const
{
#ifdef __HAIKU__
	/* We paint the frames ourselves with QPainter */
	return QWidget::paintEngine();
#else
	return nullptr;
#endif
}

void OBSQTDisplay::OnMove()
{
	if (display) {
		obs_display_update_color_space(display);
	}
}

void OBSQTDisplay::OnDisplayChange()
{
	if (display) {
		obs_display_update_color_space(display);
	}
}
