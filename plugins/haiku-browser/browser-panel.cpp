/******************************************************************************
    Copyright (C) 2026 by the OBS Studio Haiku port contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/*
 * Browser docks on Haiku: an implementation of obs-browser's QCef panel interface (browser-panel.hpp) on top
 * of the same HaikuWebKit helper the browser source uses. The frontend finds it exactly like the CEF one:
 * obs_get_module("obs-browser") -> obs_browser_create_qcef().
 */

#include "browser-client.hpp"
#include "browser-input.hpp"

#include <browser-panel.hpp>

#include <obs-module.h>
#include <util/platform.h>

#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include <memory>
#include <string>

namespace {

int32 haiku_modifiers(Qt::KeyboardModifiers mods)
{
	int32 out = 0;
	if (mods & Qt::ShiftModifier)
		out |= B_SHIFT_KEY;
	if (mods & Qt::ControlModifier)
		out |= B_COMMAND_KEY;
	if (mods & Qt::AltModifier)
		out |= B_CONTROL_KEY;
	if (mods & Qt::MetaModifier)
		out |= B_OPTION_KEY;
	return out;
}

int32 haiku_buttons(Qt::MouseButtons buttons)
{
	int32 out = 0;
	if (buttons & Qt::LeftButton)
		out |= B_PRIMARY_MOUSE_BUTTON;
	if (buttons & Qt::RightButton)
		out |= B_SECONDARY_MOUSE_BUTTON;
	if (buttons & Qt::MiddleButton)
		out |= B_TERTIARY_MOUSE_BUTTON;
	return out;
}

int32 haiku_raw_key(int qt_key)
{
	switch (qt_key) {
	case Qt::Key_Escape:
		return HAIKU_RAW_ESCAPE;
	case Qt::Key_Backspace:
		return HAIKU_RAW_BACKSPACE;
	case Qt::Key_Insert:
		return HAIKU_RAW_INSERT;
	case Qt::Key_Home:
		return HAIKU_RAW_HOME;
	case Qt::Key_PageUp:
		return HAIKU_RAW_PAGE_UP;
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
		return HAIKU_RAW_TAB;
	case Qt::Key_Delete:
		return HAIKU_RAW_DELETE;
	case Qt::Key_End:
		return HAIKU_RAW_END;
	case Qt::Key_PageDown:
		return HAIKU_RAW_PAGE_DOWN;
	case Qt::Key_Return:
		return HAIKU_RAW_ENTER;
	case Qt::Key_Enter:
		return HAIKU_RAW_KP_ENTER;
	case Qt::Key_Up:
		return HAIKU_RAW_UP;
	case Qt::Key_Left:
		return HAIKU_RAW_LEFT;
	case Qt::Key_Down:
		return HAIKU_RAW_DOWN;
	case Qt::Key_Right:
		return HAIKU_RAW_RIGHT;
	default:
		if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F12)
			return HAIKU_RAW_F1 + (qt_key - Qt::Key_F1);
		return 0;
	}
}

class HaikuCookieManager : public QCefCookieManager {
public:
	explicit HaikuCookieManager(const std::string &path) : storage_path(path) {}

	/* HaikuWebKit keeps cookies per helper process in memory; there is no store to edit. */
	bool DeleteCookies(const std::string &, const std::string &) override { return true; }
	bool SetStoragePath(const std::string &path, bool) override
	{
		storage_path = path;
		return true;
	}
	bool FlushStore() override { return true; }
	void CheckForCookie(const std::string &, const std::string &, cookie_exists_cb callback) override
	{
		if (callback)
			callback(false);
	}

	std::string storage_path;
};

class HaikuQCefWidget : public QCefWidget {
public:
	HaikuQCefWidget(QWidget *parent, const std::string &url, const std::string &storage)
		: QCefWidget(parent),
		  client(std::make_unique<HaikuBrowserClient>("dock", storage))
	{
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
		setAttribute(Qt::WA_OpaquePaintEvent);

		config.url = url.empty() ? std::string("about:blank") : url;
		config.width = std::max(width(), 1);
		config.height = std::max(height(), 1);
		config.fps = 30;
		config.transparent = false;

		timer = new QTimer(this);
		timer->setInterval(16);
		connect(timer, &QTimer::timeout, this, [this]() { Tick(); });
		timer->start();
	}

	~HaikuQCefWidget() override { closeBrowser(); }

	void setURL(const std::string &url) override
	{
		config.url = url;
		if (configured)
			client->Configure(config);
	}

	void setStartupScript(const std::string &script) override { client->SetStartupScript(script); }
	void allowAllPopups(bool) override {} /* popups always open in the same view */

	void closeBrowser() override
	{
		if (timer)
			timer->stop();
		client->Stop();
	}

	void reloadPage() override { client->Reload(false); }

	bool zoomPage(int direction) override
	{
		client->Zoom(direction);
		return true;
	}

	void executeJavaScript(const std::string &script) override { client->ExecuteJavaScript(script); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		if (image.isNull()) {
			painter.fillRect(rect(), palette().window());
			return;
		}
		painter.drawImage(0, 0, image);
		if (image.width() < width())
			painter.fillRect(image.width(), 0, width() - image.width(), height(), palette().window());
		if (image.height() < height())
			painter.fillRect(0, image.height(), image.width(), height() - image.height(),
					 palette().window());
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QCefWidget::resizeEvent(event);
		config.width = std::max(event->size().width(), 1);
		config.height = std::max(event->size().height(), 1);
		if (configured)
			client->Configure(config);
	}

	void showEvent(QShowEvent *event) override
	{
		QCefWidget::showEvent(event);
		if (!configured) {
			/* start the helper once the dock has its real size */
			configured = true;
			config.width = std::max(width(), 1);
			config.height = std::max(height(), 1);
			client->Configure(config);
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		const QPoint p = e->position().toPoint();
		clicks = (e->timestamp() - last_click < 400 && p == last_click_pos) ? clicks + 1 : 1;
		last_click = e->timestamp();
		last_click_pos = p;
		client->MouseDown(p.x(), p.y(), haiku_buttons(e->button()), clicks, haiku_modifiers(e->modifiers()));
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		const QPoint p = e->position().toPoint();
		client->MouseUp(p.x(), p.y(), haiku_buttons(e->button()), clicks, haiku_modifiers(e->modifiers()));
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override { mousePressEvent(e); }

	void mouseMoveEvent(QMouseEvent *e) override
	{
		const QPoint p = e->position().toPoint();
		client->MouseMove(p.x(), p.y(), haiku_modifiers(e->modifiers()), false);
	}

	void leaveEvent(QEvent *e) override
	{
		QCefWidget::leaveEvent(e);
		client->MouseMove(-1, -1, 0, true);
	}

	void wheelEvent(QWheelEvent *e) override
	{
		const QPoint d = e->angleDelta();
		client->MouseWheel(-d.x() / 120.0f, -d.y() / 120.0f, haiku_modifiers(e->modifiers()));
	}

	void keyPressEvent(QKeyEvent *e) override { Key(e, true); }
	void keyReleaseEvent(QKeyEvent *e) override { Key(e, false); }

	void focusInEvent(QFocusEvent *e) override
	{
		QCefWidget::focusInEvent(e);
		client->Focus(true);
	}

	void focusOutEvent(QFocusEvent *e) override
	{
		QCefWidget::focusOutEvent(e);
		client->Focus(false);
	}

	/* keep Tab inside the page instead of moving Qt focus */
	bool focusNextPrevChild(bool) override { return false; }

private:
	void Key(QKeyEvent *e, bool down)
	{
		const int32 raw = haiku_raw_key(e->key());
		const QByteArray text = e->text().toUtf8();
		client->Key(down, haiku_key_bytes(raw, text.constData()), raw, haiku_modifiers(e->modifiers()));
	}

	void Tick()
	{
		if (!configured)
			return;
		client->Poll();

		if (client->ReadFrame(frame)) {
			QImage view(frame.pixels.data(), (int)frame.width, (int)frame.height, (int)frame.stride,
				    QImage::Format_RGB32);
			image = view.copy(); /* the frame buffer is reused */
			update();
		}

		std::string title, url;
		if (client->ReadInfo(title, url)) {
			if (title != last_title) {
				last_title = title;
				emit titleChanged(QString::fromStdString(title));
			}
			if (url != last_url) {
				last_url = url;
				emit urlChanged(QString::fromStdString(url));
			}
		}
	}

	std::unique_ptr<HaikuBrowserClient> client;
	HaikuBrowserConfig config;
	bool configured = false;
	QPointer<QTimer> timer;
	HaikuBrowserFrame frame;
	QImage image;
	std::string last_title, last_url;

	int32 clicks = 1;
	quint64 last_click = 0;
	QPoint last_click_pos;
};

class HaikuQCef : public QCef {
public:
	bool init_browser(void) override { return true; }
	bool initialized(void) override { return true; }
	bool wait_for_browser_init(void) override { return true; }

	QCefWidget *create_widget(QWidget *parent, const std::string &url, QCefCookieManager *cookies) override
	{
		std::string storage;
		if (auto *haiku_cookies = dynamic_cast<HaikuCookieManager *>(cookies))
			storage = haiku_cookies->storage_path;
		return new HaikuQCefWidget(parent, url, storage);
	}

	QCefCookieManager *create_cookie_manager(const std::string &storage_path, bool) override
	{
		BPtr<char> path = get_cookie_path(storage_path);
		return new HaikuCookieManager(path ? std::string(path.Get()) : std::string());
	}

	BPtr<char> get_cookie_path(const std::string &storage_path) override
	{
		BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
		if (!rpath)
			return nullptr;
		os_mkdirs(rpath);
		return os_get_abs_path_ptr(rpath.Get());
	}

	void add_popup_whitelist_url(const std::string &, QObject *) override {}
	void add_force_popup_url(const std::string &, QObject *) override {}
};

} // namespace

extern "C" EXPORT QCef *obs_browser_create_qcef(void)
{
	return new HaikuQCef();
}

/* 3: executeJavaScript and the cookie manager are available (matches obs-browser's BROWSER_PANEL_VERSION) */
extern "C" EXPORT int obs_browser_qcef_version_export(void)
{
	return 3;
}
