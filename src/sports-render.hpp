// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "canvas.hpp"
#include <QPainterPathStroker>
#include <QPainter>
#include <cmath>
namespace program_draw {
// Trim by arc length using public APIs available before Qt 6.10. Preserve
// cubic geometry with De Casteljau subdivision instead of flattening curves.
inline QPainterPath pathPrefix(const QPainterPath &path, double length)
{
	QPainterPath result;
	QPointF current;
	auto mix = [](QPointF a, QPointF b, double t) {
		return a + (b - a) * t;
	};
	for (int i = 0; i < path.elementCount(); ++i) {
		const auto e = path.elementAt(i);
		const QPointF point(e.x, e.y);
		if (e.isMoveTo()) {
			result.moveTo(point);
			current = point;
			continue;
		}
		if (length <= 0)
			break;
		if (e.isLineTo()) {
			const double size = QLineF(current, point).length();
			if (size > length) {
				result.lineTo(mix(current, point, length / size));
				break;
			}
			result.lineTo(point);
			length -= size;
			current = point;
		} else if (e.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
			const auto b = path.elementAt(++i), c = path.elementAt(++i);
			const QPointF control2(b.x, b.y), end(c.x, c.y);
			QPainterPath segment(current);
			segment.cubicTo(point, control2, end);
			const double size = segment.length();
			if (size > length) {
				const double t = segment.percentAtLength(length);
				const auto a1 = mix(current, point, t), b1 = mix(point, control2, t),
					   c1 = mix(control2, end, t);
				const auto a2 = mix(a1, b1, t), b2 = mix(b1, c1, t);
				result.cubicTo(a1, a2, mix(a2, b2, t));
				break;
			}
			result.cubicTo(point, control2, end);
			length -= size;
			current = end;
		}
	}
	return result;
}
inline QPainterPath arrowShape(const QPainterPath &route, QPointF tip, QPointF tangent, double width, bool dashed)
{
	QPainterPathStroker stroke;
	stroke.setWidth(width);
	stroke.setCapStyle(Qt::RoundCap);
	stroke.setJoinStyle(Qt::RoundJoin);
	if (dashed)
		stroke.setDashPattern(QVector<qreal>{2.5, 2.0});
	double len = std::hypot(tangent.x(), tangent.y());
	if (len < 0.01)
		return stroke.createStroke(route);
	QPointF unit = tangent / len, normal(-unit.y(), unit.x());
	const double head = std::max(12.0, width * 3.5);
	// End the shaft inside the head; a round cap at the tip would protrude.
	const double length = route.length();
	QPainterPath shape =
		stroke.createStroke(pathPrefix(route, length * std::max(.01, 1 - head * .75 / std::max(1.0, length))));
	QPainterPath triangle;
	triangle.moveTo(tip);
	triangle.lineTo(tip - unit * head + normal * head * .46);
	triangle.lineTo(tip - unit * head - normal * head * .46);
	triangle.closeSubpath();
	return shape.united(triangle);
}
// Font-independent scoreboard numerals: identical rendering in OBS and tests.
inline void drawNumber(QPainter &p, QPointF center, double diameter, int number, QColor color)
{
	const QString digits = QString::number(number);
	static const int masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
	static const QLineF segments[] = {{{1, 0}, {7, 0}},  {{8, 1}, {8, 7}}, {{8, 9}, {8, 15}}, {{1, 16}, {7, 16}},
					  {{0, 9}, {0, 15}}, {{0, 1}, {0, 7}}, {{1, 8}, {7, 8}}};
	p.save();
	double scale = diameter * .48 / 16;
	double total = digits.size() * 12 - 4;
	p.translate(center - QPointF(total * scale / 2, 8 * scale));
	p.scale(scale, scale);
	p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
	for (const QChar digit : digits) {
		int mask = masks[digit.digitValue()];
		for (int i = 0; i < 7; ++i)
			if (mask & (1 << i))
				p.drawLine(segments[i]);
		p.translate(12, 0);
	}
	p.restore();
}
} // namespace program_draw
