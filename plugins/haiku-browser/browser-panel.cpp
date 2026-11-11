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
#include <QWheelEvent>

#include <ctype.h>
#include <dlfcn.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

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

/*
 * The cookie store behind a QCef cookie manager: the SQLite database HaikuWebKit's helpers keep in the
 * manager's storage directory (OBS_BROWSER_COOKIE_DB, see setup_cookie_store() in helper/browser-helper.cpp).
 * WebKit offers no cookie API to embedders, so the manager edits the database directly. Every helper queries
 * it per request, so changes apply to running docks at once; SQLite's locking (WAL) makes this safe while
 * helpers write.
 *
 * libsqlite3 is loaded at run time (HaikuWebKit depends on it, so it exists wherever pages can render) and only
 * a database in the layout of WebCore's CookieJarDB (schema version 1) is touched. Anything else (no
 * libsqlite3, a future WebKit with another layout) is logged once and leaves cookies alone: logins then
 * persist but cannot be cleared from OBS, and cookie checks report "not found". Cookie values are never read.
 */
namespace cookie_db {

struct sqlite3;
struct sqlite3_stmt;

/* the stable sqlite3 C ABI (https://sqlite.org/c3ref/), declared here so the build needs no sqlite headers */
constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
#define COOKIE_DB_TRANSIENT ((void (*)(void *))-1)

struct Api {
	int (*open_v2)(const char *, sqlite3 **, int, const char *);
	int (*close)(sqlite3 *);
	int (*busy_timeout)(sqlite3 *, int);
	int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
	int (*bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
	int (*bind_int64)(sqlite3_stmt *, int, long long);
	int (*step)(sqlite3_stmt *);
	int (*column_int)(sqlite3_stmt *, int);
	int (*finalize)(sqlite3_stmt *);
	const char *(*errmsg)(sqlite3 *);
};

const Api *api()
{
	static Api table;
	static bool ok = false;
	static std::once_flag once;
	std::call_once(once, []() {
		void *lib = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			lib = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
		if (!lib) {
			blog(LOG_WARNING, "[haiku-browser] libsqlite3 is not available; browser dock cookies cannot be "
					  "cleared or checked from OBS");
			return;
		}
		bool all = true;
		auto sym = [&](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
			all = all && fn;
		};
		sym(table.open_v2, "sqlite3_open_v2");
		sym(table.close, "sqlite3_close");
		sym(table.busy_timeout, "sqlite3_busy_timeout");
		sym(table.prepare_v2, "sqlite3_prepare_v2");
		sym(table.bind_text, "sqlite3_bind_text");
		sym(table.bind_int64, "sqlite3_bind_int64");
		sym(table.step, "sqlite3_step");
		sym(table.column_int, "sqlite3_column_int");
		sym(table.finalize, "sqlite3_finalize");
		sym(table.errmsg, "sqlite3_errmsg");
		if (!all) {
			blog(LOG_WARNING, "[haiku-browser] libsqlite3 lacks expected functions; browser dock cookies "
					  "cannot be cleared or checked from OBS");
			return; /* the library stays loaded; unloading buys nothing */
		}
		ok = true;
	});
	return ok ? &table : nullptr;
}

void warn_layout_once(const char *path)
{
	static std::once_flag once;
	std::call_once(once, [path]() {
		blog(LOG_WARNING,
		     "[haiku-browser] %s is not in the cookie database layout this plugin knows (WebCore "
		     "CookieJarDB schema 1); browser dock cookies cannot be cleared or checked from OBS",
		     path);
	});
}

class Statement {
public:
	Statement(const Api *a, sqlite3 *db, const char *sql) : a(a)
	{
		if (a->prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
			stmt = nullptr;
	}
	~Statement()
	{
		if (stmt)
			a->finalize(stmt);
	}
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;

	explicit operator bool() const { return stmt != nullptr; }
	void Bind(int index, const std::string &text)
	{
		a->bind_text(stmt, index, text.c_str(), (int)text.size(), COOKIE_DB_TRANSIENT);
	}
	void Bind(int index, long long value) { a->bind_int64(stmt, index, value); }
	int Step() { return a->step(stmt); }
	int Int(int column) { return a->column_int(stmt, column); }

private:
	const Api *a;
	sqlite3_stmt *stmt = nullptr;
};

/* One short-lived connection per operation: no state shared between threads or with a profile switch. */
class Database {
public:
	enum State { MISSING, READY, UNUSABLE };

	explicit Database(const std::string &dir) : path(dir + "/" OBS_BROWSER_COOKIE_DB), a(api())
	{
		struct stat st;
		if (dir.empty() || stat(path.c_str(), &st) != 0) {
			state = MISSING; /* no page has stored a cookie here yet */
			return;
		}
		if (!a)
			return;
		/* never create it: WebKit owns the file (and its layout); a helper makes it on first use */
		if (a->open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
			blog(LOG_WARNING, "[haiku-browser] cannot open %s: %s", path.c_str(),
			     db ? a->errmsg(db) : "out of memory");
			return;
		}
		/* helpers' writes are single statements and this connection lives for one operation; wait as long
		 * as the helpers do (browser-helper.cpp registers the same timeout for WebKit's connections) */
		a->busy_timeout(db, 5000);

		Statement version(a, db, "PRAGMA user_version;");
		const bool known_version = version && version.Step() == SQLITE_ROW && version.Int(0) == 1;
		Statement columns(a, db, "SELECT name, domain, path, expires, session FROM Cookie LIMIT 0;");
		if (!known_version || !columns) {
			warn_layout_once(path.c_str());
			return;
		}
		state = READY;
	}
	~Database()
	{
		if (db)
			a->close(db);
	}
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;

	bool Exec(Statement &stmt)
	{
		if (!stmt)
			return false;
		const int rc = stmt.Step();
		if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
			blog(LOG_WARNING, "[haiku-browser] cookie database %s: %s", path.c_str(), a->errmsg(db));
			return false;
		}
		return true;
	}

	State state = UNUSABLE;
	std::string path;
	const Api *a;
	sqlite3 *db = nullptr;
};

/* "https://www.twitch.tv/path", "twitch.tv" or ".twitch.tv" -> "www.twitch.tv" / "twitch.tv"; empty if it
 * is not a plain host name (IPv6 literals and the like are not cookie domains the frontend uses) */
std::string host_of(const std::string &site)
{
	std::string host = site;
	const size_t scheme = host.find("://");
	if (scheme != std::string::npos)
		host.erase(0, scheme + 3);
	host = host.substr(0, host.find_first_of("/?#"));
	const size_t at = host.rfind('@');
	if (at != std::string::npos)
		host.erase(0, at + 1);
	host = host.substr(0, host.find(':'));
	while (!host.empty() && host[0] == '.')
		host.erase(0, 1);
	for (char &c : host) {
		c = (char)tolower((unsigned char)c);
		if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_')
			return std::string();
	}
	return host;
}

/* Deletes the cookies of `site` (its host and subdomains: "twitch.tv" also clears ".twitch.tv" and
 * "id.twitch.tv") or, with an empty site, all of them; only those called `name` unless it is empty. */
bool delete_cookies(const std::string &dir, const std::string &site, const std::string &name)
{
	Database d(dir);
	if (d.state == Database::MISSING)
		return true;
	if (d.state != Database::READY)
		return false;

	std::string sql = "DELETE FROM Cookie WHERE (?1 = '' OR name = ?1)";
	std::string host;
	if (!site.empty()) {
		host = host_of(site);
		if (host.empty())
			return false;
		/* the domain column holds "host" for host-only cookies and ".domain" for domain cookies */
		sql += " AND (domain = ?2 OR substr(domain, -(length(?2) + 1)) = '.' || ?2)";
	}
	Statement stmt(d.a, d.db, sql.c_str());
	if (!stmt)
		return false;
	stmt.Bind(1, name);
	if (!host.empty())
		stmt.Bind(2, host);
	return d.Exec(stmt);
}

/* Whether a live cookie called `name` would be sent to `site` (path attributes aside). */
bool has_cookie(const std::string &dir, const std::string &site, const std::string &name)
{
	const std::string host = host_of(site);
	if (host.empty() || name.empty())
		return false;
	Database d(dir);
	if (d.state != Database::READY)
		return false;

	/* WebCore stores `expires` in milliseconds since the epoch, 0 for session cookies. Domains match as in
	 * WebCore's CookieUtil::domainMatch(): a host-only cookie ("twitch.tv") only for that exact host, a
	 * domain cookie (".twitch.tv") for the domain itself and every host under it. */
	Statement stmt(d.a, d.db,
		       "SELECT 1 FROM Cookie WHERE name = ?1 AND (session = 1 OR expires > ?3) AND "
		       "(lower(domain) = ?2 OR (substr(domain, 1, 1) = '.' AND "
		       "(lower(substr(domain, 2)) = ?2 OR substr(?2, -length(domain)) = lower(domain)))) LIMIT 1;");
	if (!stmt)
		return false;
	stmt.Bind(1, name);
	stmt.Bind(2, host);
	stmt.Bind(3, (long long)time(nullptr) * 1000LL);
	return stmt.Step() == SQLITE_ROW;
}

/* Moves what the write-ahead log holds into the database file, so a copy of the file alone (profile
 * duplication copies the directory's files) is complete. Commits are durable without it. */
bool flush(const std::string &dir)
{
	Database d(dir);
	if (d.state == Database::MISSING)
		return true;
	if (d.state != Database::READY)
		return false;
	Statement stmt(d.a, d.db, "PRAGMA wal_checkpoint(PASSIVE);");
	return d.Exec(stmt);
}

} // namespace cookie_db

class HaikuCookieManager : public QCefCookieManager {
public:
	explicit HaikuCookieManager(const std::string &path) : storage_path(path) {}

	/* `url`: a host ("twitch.tv") or URL whose cookies to delete, empty for all; `name`: empty for all names */
	bool DeleteCookies(const std::string &url, const std::string &name) override
	{
		return cookie_db::delete_cookies(storage_path, url, name);
	}

	/* Docks created afterwards use the new store; running ones keep theirs (as with CEF request contexts). Only
	 * session cookies' lifetime is WebKit's own (see HAIKU.md); `persist_session_cookies` cannot change it. */
	bool SetStoragePath(const std::string &path, bool) override
	{
		if (path.empty())
			return false;
		if (path[0] == '/') {
			storage_path = path;
		} else {
			BPtr<char> rpath = obs_module_config_path(path.c_str());
			if (!rpath)
				return false;
			os_mkdirs(rpath);
			BPtr<char> abs = os_get_abs_path_ptr(rpath.Get());
			if (!abs)
				return false;
			storage_path = abs.Get();
		}
		return true;
	}

	bool FlushStore() override { return cookie_db::flush(storage_path); }

	/* answered synchronously (the callers marshal to their own thread either way); one indexed query */
	void CheckForCookie(const std::string &site, const std::string &cookie, cookie_exists_cb callback) override
	{
		const bool found = cookie_db::has_cookie(storage_path, site, cookie);
		if (callback)
			callback(found);
	}

	std::string storage_path;
};

class HaikuQCefWidget : public QCefWidget {
public:
	HaikuQCefWidget(QWidget *parent, const std::string &url, const std::string &storage) : QCefWidget(parent)
	{
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
		setAttribute(Qt::WA_OpaquePaintEvent);

		config.url = url.empty() ? std::string("about:blank") : url;
		config.width = std::max(width(), 1);
		config.height = std::max(height(), 1);
		config.fps = 30;
		config.transparent = false;

		/*
		 * The callbacks run on the client's I/O thread. Frames are handed over through a one-slot mailbox
		 * (`latest`): the I/O thread replaces what is there and queues one call to the UI thread only if none
		 * is pending, so a busy UI thread drops intermediate frames instead of collecting a backlog. Queued
		 * calls whose context object (`this`) was deleted are discarded by Qt; the client, and with it the
		 * I/O thread, is destroyed before the widget's QObject part (it is a member).
		 */
		HaikuBrowserClient::Callbacks callbacks;
		callbacks.frame = [this](const HaikuBrowserFrame &frame) {
			QImage view(frame.pixels, (int)frame.width, (int)frame.height, (int)frame.stride,
				    QImage::Format_RGB32);
			QImage copy = view.copy(); /* the frame buffer is reused */
			bool post;
			{
				std::lock_guard<std::mutex> lock(latest_mutex);
				latest = std::move(copy);
				post = !frame_posted;
				frame_posted = true;
			}
			if (post)
				QMetaObject::invokeMethod(this, [this]() { TakeFrame(); }, Qt::QueuedConnection);
		};
		callbacks.info = [this](const std::string &title, const std::string &url) {
			const QString t = QString::fromStdString(title), u = QString::fromStdString(url);
			QMetaObject::invokeMethod(
				this, [this, t, u]() { Info(t, u); }, Qt::QueuedConnection);
		};
		client = std::make_unique<HaikuBrowserClient>("dock", std::move(callbacks), storage);
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

	/* Stop() returns after the last callback; it never waits for the UI thread (the callbacks only post) */
	void closeBrowser() override { client->Stop(); }

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

	void TakeFrame()
	{
		{
			std::lock_guard<std::mutex> lock(latest_mutex);
			image = std::move(latest);
			latest = QImage();
			frame_posted = false;
		}
		update();
	}

	void Info(const QString &title, const QString &url)
	{
		if (title != last_title) {
			last_title = title;
			emit titleChanged(title);
		}
		if (url != last_url) {
			last_url = url;
			emit urlChanged(url);
		}
	}

	/* declared first, destroyed last: the callbacks below use these until the client is gone */
	std::mutex latest_mutex;
	QImage latest;
	bool frame_posted = false;

	HaikuBrowserConfig config;
	bool configured = false;
	QImage image;
	QString last_title, last_url;

	std::unique_ptr<HaikuBrowserClient> client;

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
