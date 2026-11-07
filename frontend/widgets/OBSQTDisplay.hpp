#pragma once

#include <obs.hpp>

#include <QWidget>

#ifdef __HAIKU__
#include <QImage>

#include <atomic>
#include <mutex>
#endif

#define GREY_COLOR_BACKGROUND 0xFF4C4C4C

class OBSQTDisplay : public QWidget {
	Q_OBJECT
	Q_PROPERTY(QColor displayBackgroundColor MEMBER backgroundColor READ GetDisplayBackgroundColor WRITE
			   SetDisplayBackgroundColor)

	OBSDisplay display;
	bool destroying = false;

#ifdef __HAIKU__
	/* libobs renders Haiku swapchains offscreen and hands every frame to us (see gl-haiku.c). */
	std::mutex frameMutex;
	QImage frame;
	std::atomic_bool frameUpdatePending = false;

	static void HaikuPresent(void *param, const uint8_t *bgra, uint32_t width, uint32_t height,
				 uint32_t linesize);
#endif

protected:
	virtual void paintEvent(QPaintEvent *event) override;
	virtual void moveEvent(QMoveEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;
	virtual bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#ifdef __HAIKU__
	virtual void showEvent(QShowEvent *event) override;
	virtual void hideEvent(QHideEvent *event) override;
#endif

signals:
	void DisplayCreated(OBSQTDisplay *window);
	void DisplayResized();

public:
	OBSQTDisplay(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~OBSQTDisplay() { display = nullptr; }

	virtual QPaintEngine *paintEngine() const override;

	inline obs_display_t *GetDisplay() const { return display; }

	uint32_t backgroundColor = GREY_COLOR_BACKGROUND;

	QColor GetDisplayBackgroundColor() const;
	void SetDisplayBackgroundColor(const QColor &color);
	void UpdateDisplayBackgroundColor();
	void CreateDisplay();
	void DestroyDisplay()
	{
		display = nullptr;
		destroying = true;
	};

	void OnMove();
	void OnDisplayChange();
};
