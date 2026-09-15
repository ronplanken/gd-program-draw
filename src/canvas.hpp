// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>
#include <utility>
namespace program_draw {
QRectF programVideoRect(QSize logicalSize, double dpr, QSize canvasSize);
std::optional<QPointF> mapToCanvas(QPointF point, QSize logicalSize, double dpr, QSize canvasSize);
enum class Tool {
	None,
	Pencil,
	Brush,
	Line,
	RectangleOutline,
	RectangleFill,
	EllipseOutline,
	EllipseFill,
	SelectRectangle,
	SelectEllipse,
	Stamp,
	Image,
	Highlighter,
	Arrow,
	CurvedArrow,
	DashedRoute,
	Spotlight,
	PlayerRing,
	NumberMarker
};
inline bool isCursorTool(Tool tool)
{
	return tool == Tool::Highlighter || tool == Tool::Spotlight;
}
struct PaintStyle {
	Tool tool = Tool::Pencil;
	QColor color = Qt::red;
	double width = 8, opacity = 1;
	QImage asset;
	double imageWidth = 160;
	double curvature = 35, markerSize = 64;
	bool contrastOutline = true;
};
class Canvas {
public:
	struct Snapshot {
		QImage image;
		uint64_t revision;
		bool empty;
	};
	struct LayerInfo {
		QString name;
		bool visible;
		double opacity;
		bool active;
		bool locked;
	};
	struct SelectionInfo {
		QRectF rect;
		bool ellipse = false;
	};
	void resize(QSize size);
	QSize size() const;
	bool begin(QPointF point, QColor color, double width);
	bool begin(QPointF point, const PaintStyle &style);
	void extend(QPointF point, bool constrain = false);
	void finish();
	void cancel();
	void breakSegment();
	std::pair<bool, bool> historyAvailable() const;
	void undo();
	void redo();
	void clear();
	void clearAll();
	void clearLayer();
	void deselect();
	void deleteSelection();
	SelectionInfo selection() const;
	QImage selectionImage() const;
	std::vector<LayerInfo> layers() const;
	int activeLayer() const;
	int nextMarker() const;
	void setNextMarker(int number);
	void clearSpotlight();
	void selectLayer(int index);
	bool addLayer();
	bool removeLayer();
	void renameLayer(const QString &name);
	void moveLayer(int direction);
	void moveLayerTo(int index);
	void setLayerLocked(bool locked);
	void setLayerVisible(bool visible);
	void setLayerOpacity(double opacity);
	Snapshot snapshot(uint64_t lastRevision) const;

private:
	struct Layer {
		QString name;
		bool visible = true;
		double opacity = 1;
		QImage image;
		bool ink = false;
		QImage spotlight;
		bool locked = false;
	};
	struct State {
		std::vector<Layer> layers;
		int active = 0;
		int nextMarker = 1;
	};
	enum class Gesture { Idle, Paint, Select, Move };
	void pushUndo(const State &state);
	void moveLayerToLocked(int index);
	void finishLocked();
	void cancelLocked();
	void resetSelection();
	void renderGesture(bool constrain);
	void captureSelection();
	QPainterPath selectionPath() const;
	QImage blank() const;
	mutable std::mutex mutex_;
	QSize size_{1920, 1080};
	State state_, before_;
	std::vector<State> undo_, redo_;
	Gesture gesture_ = Gesture::Idle;
	PaintStyle style_;
	QPointF start_, end_, moveOrigin_;
	QImage base_, selectedPixels_;
	SelectionInfo selection_;
	std::vector<std::optional<QPointF>> points_;
	uint64_t revision_ = 1;
	int layerNumber_ = 1;
	mutable uint64_t composedRevision_ = 0;
	mutable QImage composed_;
};
} // namespace program_draw
