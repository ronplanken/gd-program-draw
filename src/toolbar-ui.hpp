// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "canvas.hpp"
#include "lucide-icons.hpp"
#include <QSvgRenderer>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QFrame>
#include <QKeyEvent>
#include <QPushButton>
#include <QToolButton>
#include <QScreen>
#include <QGuiApplication>
#include <QVBoxLayout>
#include <QLabel>
#include <algorithm>
#include <array>

namespace program_draw {
inline const QStringList ToolNames = {"None",
				      "Pencil",
				      "Brush",
				      "Line",
				      "Rectangle Outline",
				      "Rectangle Fill",
				      "Ellipse Outline",
				      "Ellipse Fill",
				      "Select Rectangle",
				      "Select Ellipse",
				      "Stamp",
				      "Image",
				      "Highlighter",
				      "Arrow",
				      "Curved Arrow",
				      "Dashed Route",
				      "Spotlight",
				      "Player Ring",
				      "Number Marker"};
inline constexpr std::array ShapeTools = {Tool::Line, Tool::RectangleOutline, Tool::RectangleFill, Tool::EllipseOutline,
					  Tool::EllipseFill};
inline bool isShapeTool(Tool tool)
{
	return std::find(ShapeTools.begin(), ShapeTools.end(), tool) != ShapeTools.end();
}
// Lucide SVGs are bundled and rasterized with OBS's QtSvg at Retina density.
// The filled and flattened variants retain the upstream geometry.
inline QIcon toolbarIcon(int kind, QColor color)
{
	if (kind < 0 || kind >= int(LucideSvg.size()))
		return {};
	auto render = [kind](QColor ink) {
		QByteArray svg(LucideSvg[kind]);
		svg.replace("currentColor", ink.name().toUtf8());
		if (kind == 5 || kind == 7 || kind == 29)
			svg.replace("fill=\"none\"", QByteArray("fill=\"") + ink.name().toUtf8() + "\"");
		QSvgRenderer renderer(svg);
		QPixmap pm(48, 48);
		pm.fill(Qt::transparent);
		pm.setDevicePixelRatio(2);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		if (kind == 6 || kind == 7 || kind == 9) {
			p.translate(0, 3);
			p.scale(1, .75);
		}
		if (kind == 26) {
			p.translate(0, 7);
			p.scale(1, 5.0 / 12.0);
		}
		renderer.render(&p, QRectF(0, 0, 24, 24));
		return pm;
	};
	QIcon icon(render(color));
	const auto selected = QGuiApplication::palette().color(QPalette::HighlightedText);
	icon.addPixmap(render(selected), QIcon::Normal, QIcon::On);
	icon.addPixmap(render(selected), QIcon::Selected, QIcon::Off);
	icon.addPixmap(render(selected), QIcon::Selected, QIcon::On);
	return icon;
}
// Use one indicator for both a custom popover and a native menu.
class DropdownToolButton final : public QToolButton {
public:
	explicit DropdownToolButton(QWidget *parent) : QToolButton(parent)
	{
		setStyleSheet(
			"QToolButton { padding-right: 23px; } QToolButton::menu-indicator { image: none; width: 0px; height: 0px; }");
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QToolButton::paintEvent(event);
		QPainter painter(this);
		const auto ink =
			palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText);
		if (ink != chevronColor_) {
			chevronColor_ = ink;
			chevron_ = toolbarIcon(34, ink);
		}
		chevron_.paint(&painter, QRect(width() - 21, (height() - 16) / 2, 16, 16));
	}

private:
	QColor chevronColor_;
	QIcon chevron_;
};
class ToolbarPopover final : public QFrame {
public:
	explicit ToolbarPopover(QWidget *owner) : QFrame(owner, Qt::Popup | Qt::FramelessWindowHint)
	{
		// An outside click dismisses the popup; do not replay it as a drawing
		// gesture or reopen the popup when its toolbar button was clicked.
		setAttribute(Qt::WA_NoMouseReplay);
		setFocusPolicy(Qt::StrongFocus);
	}

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Escape) {
			hide();
			event->accept();
			return;
		}
		QFrame::keyPressEvent(event);
	}
};
inline ToolbarPopover *makePopover(QWidget *owner, const QString &title)
{
	auto *panel = new ToolbarPopover(owner);
	panel->setObjectName("programDrawPopover");
	panel->setWindowTitle(title);
	panel->setAccessibleName(title);
	panel->setAttribute(Qt::WA_DeleteOnClose, false);
	panel->setStyleSheet(
		"QFrame#programDrawPopover { border: 1px solid palette(mid); border-radius: 8px; background: palette(window); } QFrame#programDrawPopover QPushButton { padding: 6px 10px; } QListWidget { border: 1px solid palette(mid); border-radius: 4px; } QListWidget::item { padding: 8px; }");
	auto *layout = new QVBoxLayout(panel);
	layout->setContentsMargins(16, 14, 16, 14);
	layout->setSpacing(12);
	auto *heading = new QLabel(title, panel);
	QFont font = heading->font();
	font.setBold(true);
	heading->setFont(font);
	layout->addWidget(heading);
	return panel;
}
inline void showPopover(ToolbarPopover *panel, QWidget *anchor)
{
	if (panel->isVisible()) {
		panel->hide();
		return;
	}
	panel->adjustSize();
	const QPoint above = anchor->mapToGlobal(QPoint(0, 0));
	auto *screen = QGuiApplication::screenAt(above);
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	const QRect bounds = screen->availableGeometry();
	int x = std::clamp(above.x(), bounds.left() + 8,
			   std::max(bounds.left() + 8, bounds.right() - panel->width() - 8));
	int y = above.y() - panel->height() - 8;
	if (y < bounds.top() + 8)
		y = above.y() + anchor->height() + 8;
	y = std::clamp(y, bounds.top() + 8, std::max(bounds.top() + 8, bounds.bottom() - panel->height() - 8));
	panel->move(x, y);
	panel->show();
	panel->raise();
	panel->setFocus();
}
} // namespace program_draw
