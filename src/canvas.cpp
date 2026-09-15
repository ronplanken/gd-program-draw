// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas.hpp"
#include "sports-render.hpp"
#include <QPainter>
#include <QPen>
#include <cmath>
#include <algorithm>
#include <set>

namespace program_draw {
QRectF programVideoRect(QSize logicalSize, double dpr, QSize canvasSize)
{
	if (dpr <= 0 || canvasSize.isEmpty())
		return {};
	const QSize physical = logicalSize * dpr;
	const int w = physical.width() - 20, h = physical.height() - 20;
	if (w <= 0 || h <= 0)
		return {};
	const double aspect = double(canvasSize.width()) / canvasSize.height();
	int fitW, fitH;
	float scale;
	if (double(w) / h > aspect) {
		scale = float(h) / float(canvasSize.height());
		fitW = int(double(h) * aspect);
		fitH = h;
	} else {
		scale = float(w) / float(canvasSize.width());
		fitW = w;
		fitH = int(float(w) / aspect);
	}
	return QRectF((w / 2 - fitW / 2 + 10) / dpr, (h / 2 - fitH / 2 + 10) / dpr,
		      int(scale * float(canvasSize.width())) / dpr, int(scale * float(canvasSize.height())) / dpr);
}

std::optional<QPointF> mapToCanvas(QPointF point, QSize logicalSize, double dpr, QSize canvasSize)
{
	const auto rect = programVideoRect(logicalSize, dpr, canvasSize);
	if (rect.isEmpty() || !rect.contains(point))
		return std::nullopt;
	return QPointF((point.x() - rect.x()) * canvasSize.width() / rect.width(),
		       (point.y() - rect.y()) * canvasSize.height() / rect.height());
}

QImage Canvas::blank() const
{
	QImage result(size_, QImage::Format_RGBA8888_Premultiplied);
	result.fill(Qt::transparent);
	return result;
}
void Canvas::resize(QSize size)
{
	std::lock_guard lock(mutex_);
	if (size.isEmpty() || (size == size_ && !state_.layers.empty()))
		return;
	size_ = size;
	state_ = {};
	layerNumber_ = 1;
	state_.layers.push_back({"Layer 1", true, 1, blank(), false, {}});
	undo_.clear();
	redo_.clear();
	before_ = {};
	base_ = {};
	gesture_ = Gesture::Idle;
	resetSelection();
	++revision_;
}
QSize Canvas::size() const
{
	std::lock_guard lock(mutex_);
	return size_;
}
void Canvas::resetSelection()
{
	selection_ = {};
	selectedPixels_ = {};
}
void Canvas::pushUndo(const State &state)
{
	undo_.push_back(state);
	redo_.clear();
	// Qt images share until changed. Limit retained history to 256 MiB,
	// retaining at least one previous operation even on very large canvases.
	auto bytes = [&] {
		std::set<qint64> seen;
		qsizetype result = 0;
		for (const auto &s : undo_)
			for (const auto &l : s.layers) {
				if (seen.insert(l.image.cacheKey()).second)
					result += l.image.sizeInBytes();
				if (!l.spotlight.isNull() && seen.insert(l.spotlight.cacheKey()).second)
					result += l.spotlight.sizeInBytes();
			}
		return result;
	};
	while (undo_.size() > 1 && (undo_.size() > 32 || bytes() > 256 * 1024 * 1024))
		undo_.erase(undo_.begin());
}
bool Canvas::begin(QPointF point, QColor color, double width)
{
	PaintStyle style;
	style.color = color;
	style.width = width;
	return begin(point, style);
}
bool Canvas::begin(QPointF point, const PaintStyle &style)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || style.tool == Tool::None || state_.layers[state_.active].locked ||
	    !state_.layers[state_.active].visible)
		return false;
	if ((style.tool == Tool::Stamp || style.tool == Tool::Image) && style.asset.isNull())
		return false;
	style_ = style;
	style_.opacity = std::clamp(style.opacity, 0.0, 1.0);
	style_.width = std::clamp(style.width, 1.0, 256.0);
	before_ = state_;
	base_ = state_.layers[state_.active].image;
	start_ = end_ = point;
	points_ = {point};
	if (style.tool == Tool::SelectRectangle || style.tool == Tool::SelectEllipse) {
		if (!selection_.rect.isEmpty() && selectionPath().contains(point)) {
			captureSelection();
			moveOrigin_ = selection_.rect.topLeft();
			gesture_ = Gesture::Move;
			QPainter painter(&base_);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setCompositionMode(QPainter::CompositionMode_Clear);
			painter.fillPath(selectionPath(), Qt::white);
		} else {
			resetSelection();
			gesture_ = Gesture::Select;
			selection_.ellipse = style.tool == Tool::SelectEllipse;
		}
	} else {
		if (!isCursorTool(style.tool))
			resetSelection();
		gesture_ = Gesture::Paint;
		renderGesture(false);
	}
	return true;
}
void Canvas::extend(QPointF point, bool constrain)
{
	std::lock_guard lock(mutex_);
	if (gesture_ == Gesture::Idle)
		return;
	end_ = point;
	if (gesture_ == Gesture::Select) {
		selection_.rect = QRectF(start_, end_).normalized().intersected(QRectF(QPointF(), size_));
		return;
	}
	if (gesture_ == Gesture::Move) {
		const QPointF delta = (end_ - start_).toPoint();
		selection_.rect.moveTopLeft(moveOrigin_ + delta);
		auto &layer = state_.layers[state_.active];
		layer.image = base_;
		QPainter p(&layer.image);
		p.drawImage(selection_.rect.topLeft(), selectedPixels_);
		++revision_;
		return;
	}
	if (style_.tool == Tool::Pencil || style_.tool == Tool::Brush || style_.tool == Tool::DashedRoute) {
		if (points_.size() >= 16384)
			return;
		if (points_.back() && QLineF(*points_.back(), point).length() < 0.5)
			return;
		points_.push_back(point);
	}
	renderGesture(constrain);
}
void Canvas::renderGesture(bool constrain)
{
	auto &layer = state_.layers[state_.active];
	layer.image = base_;
	const bool sports = style_.tool >= Tool::Highlighter;
	QImage sportsInk;
	if (sports)
		sportsInk = blank();
	QPainter p(sports ? &sportsInk : &layer.image);
	p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	p.setOpacity(sports ? 1.0 : style_.opacity);
	p.setPen(QPen(style_.color, style_.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	QPointF end = end_;
	if (constrain) {
		QPointF d = end - start_;
		if (style_.tool == Tool::Line || style_.tool == Tool::Arrow || style_.tool == Tool::CurvedArrow) {
			double angle = std::round(std::atan2(d.y(), d.x()) / (3.14159265358979323846 / 4)) *
				       (3.14159265358979323846 / 4);
			double length = std::hypot(d.x(), d.y());
			end = start_ + QPointF(std::cos(angle) * length, std::sin(angle) * length);
		} else {
			double extent = std::max(std::abs(d.x()), std::abs(d.y()));
			end = start_ + QPointF(std::copysign(extent, d.x()), std::copysign(extent, d.y()));
		}
	}
	const QRectF rect = QRectF(start_, end).normalized();
	switch (style_.tool) {
	case Tool::Pencil:
	case Tool::Brush: {
		QPainterPath path;
		std::vector<QPointF> segment;
		auto flush = [&] {
			if (segment.empty())
				return;
			path.moveTo(segment[0]);
			if (segment.size() == 1)
				path.lineTo(segment[0] + QPointF(0.001, 0));
			else if ((style_.tool == Tool::Brush) && segment.size() > 2) {
				for (size_t i = 1; i + 1 < segment.size(); ++i)
					path.quadTo(segment[i], (segment[i] + segment[i + 1]) / 2);
				path.lineTo(segment.back());
			} else
				for (size_t i = 1; i < segment.size(); ++i)
					path.lineTo(segment[i]);
			segment.clear();
		};
		// One paint per gesture preview: opacity does not accumulate per sample.
		if (points_.size() == 1) {
			p.setPen(Qt::NoPen);
			p.setBrush(style_.color);
			p.drawEllipse(start_, style_.width / 2, style_.width / 2);
		} else {
			for (const auto &point : points_) {
				if (point)
					segment.push_back(*point);
				else
					flush();
			}
			flush();
			p.drawPath(path);
		}
		break;
	}
	case Tool::Highlighter:
		p.setPen(Qt::NoPen);
		p.setBrush(style_.color);
		p.drawEllipse(end_, style_.width / 2, style_.width / 2);
		break;
	case Tool::Arrow:
	case Tool::CurvedArrow:
	case Tool::DashedRoute: {
		if (QLineF(start_, end).length() < 2 && points_.size() < 2)
			break;
		QPainterPath path;
		QPointF tangent = end - start_;
		if (style_.tool == Tool::DashedRoute) {
			bool connected = false;
			QPointF previous;
			for (const auto &point : points_) {
				if (!point) {
					connected = false;
					continue;
				}
				if (!connected) {
					path.moveTo(*point);
					connected = true;
				} else {
					path.lineTo(*point);
					tangent = *point - previous;
				}
				previous = *point;
			}
			end = end_;
		} else {
			path.moveTo(start_);
			if (style_.tool == Tool::CurvedArrow) {
				QPointF d = end - start_;
				QPointF control =
					(start_ + end) / 2 +
					QPointF(-d.y(), d.x()) * std::clamp(style_.curvature, -100.0, 100.0) / 100;
				path.quadTo(control, end);
				tangent = end - control;
			} else
				path.lineTo(end);
		}
		auto shape = arrowShape(path, end, tangent, style_.width, style_.tool == Tool::DashedRoute);
		p.setPen(style_.contrastOutline ? QPen(QColor(12, 17, 24), std::max(2.0, style_.width * .45),
						       Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
						: QPen(Qt::NoPen));
		p.setBrush(style_.color);
		p.drawPath(shape);
		break;
	}
	case Tool::Spotlight: {
		QPainterPath mask;
		mask.setFillRule(Qt::OddEvenFill);
		mask.addRect(QRectF(QPointF(), size_));
		mask.addEllipse(end_, style_.width / 2, style_.width / 2);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::black);
		p.drawPath(mask);
		break;
	}
	case Tool::PlayerRing: {
		QRectF ring = rect;
		if (QLineF(start_, end).length() < 3)
			ring = QRectF(end - QPointF(60, 18), QSizeF(120, 36));
		if (style_.contrastOutline) {
			p.setPen(QPen(QColor(12, 17, 24), style_.width + 5));
			p.drawEllipse(ring);
		}
		QColor fill = style_.color;
		fill.setAlphaF(fill.alphaF() * .16);
		p.setBrush(fill);
		p.setPen(QPen(style_.color, style_.width));
		p.drawEllipse(ring);
		break;
	}
	case Tool::NumberMarker: {
		const double size = std::clamp(style_.markerSize, 24.0, 256.0);
		p.setPen(style_.contrastOutline ? QPen(QColor(12, 17, 24), 3) : QPen(Qt::NoPen));
		p.setBrush(style_.color);
		p.drawEllipse(end_, size / 2, size / 2);
		drawNumber(p, end_, size, state_.nextMarker,
			   qGray(style_.color.rgb()) > 145 ? QColor(12, 17, 24) : Qt::white);
		break;
	}
	case Tool::Line:
		p.drawLine(start_, end);
		break;
	case Tool::RectangleOutline:
		p.drawRect(rect);
		break;
	case Tool::EllipseOutline:
		p.drawEllipse(rect);
		break;
	case Tool::RectangleFill:
	case Tool::EllipseFill:
		p.setPen(Qt::NoPen);
		p.setBrush(style_.color);
		if (style_.tool == Tool::RectangleFill)
			p.drawRect(rect);
		else
			p.drawEllipse(rect);
		break;
	case Tool::Stamp: {
		double w = std::max(1.0, style_.imageWidth);
		double h = w * style_.asset.height() / style_.asset.width();
		p.drawImage(QRectF(end_.x() - w / 2, end_.y() - h / 2, w, h), style_.asset);
		break;
	}
	case Tool::Image: {
		double w = std::max(1.0, style_.imageWidth);
		double h = w * style_.asset.height() / style_.asset.width();
		QRectF destination(start_, QSizeF(w, h));
		if (QLineF(start_, end_).length() >= 3) {
			QRectF box = QRectF(start_, end_).normalized();
			QSizeF fitted = style_.asset.size();
			fitted.scale(box.size(), Qt::KeepAspectRatio);
			destination = QRectF(box.topLeft(), fitted);
		}
		p.drawImage(destination, style_.asset);
		break;
	}
	default:
		break;
	}
	if (sports) {
		p.end();
		if (style_.tool == Tool::Spotlight) {
			layer.spotlight = blank();
			QPainter out(&layer.spotlight);
			out.setOpacity(style_.opacity);
			out.drawImage(0, 0, sportsInk);
		} else {
			QPainter out(&layer.image);
			out.setOpacity(style_.opacity);
			out.drawImage(0, 0, sportsInk);
			layer.ink = true;
		}
	} else
		layer.ink = true;
	++revision_;
}
void Canvas::finishLocked()
{
	// A held cursor highlight is only a live preview. Restore the exact layer
	// state on release without creating history or discarding the redo stack.
	const bool transient = gesture_ == Gesture::Paint && isCursorTool(style_.tool);
	if (transient) {
		state_ = before_;
		++revision_;
	} else if (gesture_ == Gesture::Paint || gesture_ == Gesture::Move)
		pushUndo(before_);
	if (gesture_ == Gesture::Paint && style_.tool == Tool::NumberMarker)
		state_.nextMarker = state_.nextMarker % 99 + 1;
	if (gesture_ == Gesture::Select) {
		selection_.rect = selection_.rect.toAlignedRect();
		captureSelection();
	}
	gesture_ = Gesture::Idle;
	before_ = {};
	base_ = {};
	points_.clear();
	style_.asset = {};
}
void Canvas::finish()
{
	std::lock_guard lock(mutex_);
	finishLocked();
}
void Canvas::cancelLocked()
{
	if (gesture_ == Gesture::Paint || gesture_ == Gesture::Move) {
		state_ = before_;
		++revision_;
	}
	gesture_ = Gesture::Idle;
	before_ = {};
	base_ = {};
	points_.clear();
	resetSelection();
	style_.asset = {};
}
void Canvas::cancel()
{
	std::lock_guard lock(mutex_);
	cancelLocked();
}
void Canvas::breakSegment()
{
	std::lock_guard lock(mutex_);
	if (gesture_ == Gesture::Paint && isCursorTool(style_.tool)) {
		state_ = before_;
		++revision_;
		return;
	}
	if (gesture_ == Gesture::Paint && !points_.empty() && points_.back())
		points_.push_back(std::nullopt);
}
QPainterPath Canvas::selectionPath() const
{
	QPainterPath path;
	if (selection_.ellipse)
		path.addEllipse(selection_.rect);
	else
		path.addRect(selection_.rect);
	return path;
}
void Canvas::captureSelection()
{
	if (selection_.rect.isEmpty() || state_.layers.empty()) {
		selectedPixels_ = {};
		return;
	}
	QRect crop = selection_.rect.toAlignedRect();
	selectedPixels_ = state_.layers[state_.active].image.copy(crop);
	if (selection_.ellipse) {
		QImage mask(crop.size(), QImage::Format_RGBA8888_Premultiplied);
		mask.fill(Qt::transparent);
		{
			QPainter p(&mask);
			p.setRenderHint(QPainter::Antialiasing);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawEllipse(mask.rect());
		}
		QPainter p(&selectedPixels_);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(0, 0, mask);
	}
}
void Canvas::deselect()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	resetSelection();
}
Canvas::SelectionInfo Canvas::selection() const
{
	std::lock_guard lock(mutex_);
	return selection_;
}
QImage Canvas::selectionImage() const
{
	std::lock_guard lock(mutex_);
	return selectedPixels_;
}
void Canvas::deleteSelection()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (selection_.rect.isEmpty() || state_.layers.empty() || state_.layers[state_.active].locked ||
	    !state_.layers[state_.active].visible)
		return;
	pushUndo(state_);
	QPainter p(&state_.layers[state_.active].image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setCompositionMode(QPainter::CompositionMode_Clear);
	p.fillPath(selectionPath(), Qt::white);
	resetSelection();
	++revision_;
}
std::pair<bool, bool> Canvas::historyAvailable() const
{
	std::lock_guard lock(mutex_);
	return {!undo_.empty(), !redo_.empty()};
}
void Canvas::undo()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (undo_.empty())
		return;
	redo_.push_back(state_);
	state_ = undo_.back();
	undo_.pop_back();
	resetSelection();
	++revision_;
}
void Canvas::redo()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (redo_.empty())
		return;
	undo_.push_back(state_);
	state_ = redo_.back();
	redo_.pop_back();
	resetSelection();
	++revision_;
}
void Canvas::clear()
{
	std::lock_guard lock(mutex_);
	cancelLocked();
	// Lifecycle clears forget undo: changing scene cannot resurrect old ink.
	for (auto &l : state_.layers) {
		if (l.locked)
			continue;
		l.image = blank();
		l.ink = false;
		l.spotlight = {};
	}
	state_.nextMarker = 1;
	undo_.clear();
	redo_.clear();
	++revision_;
}
void Canvas::clearLayer()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers[state_.active].locked)
		return;
	pushUndo(state_);
	auto &l = state_.layers[state_.active];
	l.image = blank();
	l.ink = false;
	l.spotlight = {};
	resetSelection();
	++revision_;
}
void Canvas::clearAll()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (std::none_of(state_.layers.begin(), state_.layers.end(), [](const Layer &l) { return !l.locked; }))
		return;
	pushUndo(state_);
	for (auto &l : state_.layers) {
		if (l.locked)
			continue;
		l.image = blank();
		l.ink = false;
		l.spotlight = {};
	}
	resetSelection();
	++revision_;
}
std::vector<Canvas::LayerInfo> Canvas::layers() const
{
	std::lock_guard lock(mutex_);
	std::vector<LayerInfo> result;
	for (int i = 0; i < int(state_.layers.size()); ++i) {
		const auto &l = state_.layers[i];
		result.push_back({l.name, l.visible, l.opacity, i == state_.active, l.locked});
	}
	return result;
}
int Canvas::nextMarker() const
{
	std::lock_guard lock(mutex_);
	return state_.nextMarker;
}
void Canvas::setNextMarker(int number)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	number = std::clamp(number, 1, 99);
	if (state_.nextMarker == number)
		return;
	pushUndo(state_);
	state_.nextMarker = number;
}
void Canvas::clearSpotlight()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers[state_.active].locked ||
	    state_.layers[state_.active].spotlight.isNull())
		return;
	pushUndo(state_);
	state_.layers[state_.active].spotlight = {};
	++revision_;
}
int Canvas::activeLayer() const
{
	std::lock_guard lock(mutex_);
	return state_.active;
}
void Canvas::selectLayer(int index)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (index >= 0 && index < int(state_.layers.size())) {
		state_.active = index;
		resetSelection();
	}
}
bool Canvas::addLayer()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers.size() >= 8)
		return false;
	pushUndo(state_);
	state_.layers.insert(state_.layers.begin() + state_.active + 1,
			     {QString("Layer %1").arg(++layerNumber_), true, 1, blank(), false, {}});
	++state_.active;
	resetSelection();
	++revision_;
	return true;
}
bool Canvas::removeLayer()
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.size() <= 1 || state_.layers[state_.active].locked)
		return false;
	pushUndo(state_);
	state_.layers.erase(state_.layers.begin() + state_.active);
	state_.active = std::min(state_.active, int(state_.layers.size()) - 1);
	resetSelection();
	++revision_;
	return true;
}
void Canvas::renameLayer(const QString &name)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers[state_.active].locked || name.trimmed().isEmpty())
		return;
	pushUndo(state_);
	state_.layers[state_.active].name = name.trimmed().left(64);
}
void Canvas::moveLayer(int direction)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	moveLayerToLocked(state_.active + (direction > 0 ? 1 : -1));
}
void Canvas::moveLayerTo(int index)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	moveLayerToLocked(index);
}
void Canvas::moveLayerToLocked(int index)
{
	if (index < 0 || index >= int(state_.layers.size()) || index == state_.active ||
	    state_.layers[state_.active].locked)
		return;
	pushUndo(state_);
	auto layer = std::move(state_.layers[state_.active]);
	state_.layers.erase(state_.layers.begin() + state_.active);
	state_.layers.insert(state_.layers.begin() + index, std::move(layer));
	state_.active = index;
	resetSelection();
	++revision_;
}
void Canvas::setLayerLocked(bool locked)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers[state_.active].locked == locked)
		return;
	pushUndo(state_);
	state_.layers[state_.active].locked = locked;
	resetSelection();
}
void Canvas::setLayerVisible(bool visible)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	if (state_.layers.empty() || state_.layers[state_.active].visible == visible)
		return;
	pushUndo(state_);
	state_.layers[state_.active].visible = visible;
	resetSelection();
	++revision_;
}
void Canvas::setLayerOpacity(double opacity)
{
	std::lock_guard lock(mutex_);
	finishLocked();
	opacity = std::clamp(opacity, 0.0, 1.0);
	if (state_.layers.empty() || state_.layers[state_.active].locked ||
	    state_.layers[state_.active].opacity == opacity)
		return;
	pushUndo(state_);
	state_.layers[state_.active].opacity = opacity;
	++revision_;
}
Canvas::Snapshot Canvas::snapshot(uint64_t lastRevision) const
{
	std::lock_guard lock(mutex_);
	bool empty = true;
	for (const auto &l : state_.layers)
		if (l.visible && l.opacity > 0 && (l.ink || !l.spotlight.isNull()))
			empty = false;
	if (lastRevision == revision_)
		return {{}, revision_, empty};
	if (composedRevision_ != revision_) {
		composed_ = blank();
		QPainter p(&composed_);
		for (const auto &l : state_.layers)
			if (l.visible && l.opacity > 0 && (l.ink || !l.spotlight.isNull())) {
				p.setOpacity(l.opacity);
				if (!l.spotlight.isNull())
					p.drawImage(0, 0, l.spotlight);
				p.drawImage(0, 0, l.image);
			}
		composedRevision_ = revision_;
	}
	return {composed_, revision_, empty};
}
} // namespace program_draw
