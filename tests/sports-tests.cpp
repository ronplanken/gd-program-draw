// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas.hpp"
#include "sports-render.hpp"
#include <QPainter>
#include <iostream>
#include <stdexcept>
using namespace program_draw;
static void check(bool ok, const char *why)
{
	if (!ok)
		throw std::runtime_error(why);
}
static QColor at(Canvas &c, int x, int y)
{
	return c.snapshot(0).image.pixelColor(x, y);
}
static void draw(Canvas &c, PaintStyle s, QPointF a, QPointF b)
{
	check(c.begin(a, s), "begin sports gesture");
	c.extend(b);
	c.finish();
}
int main(int argc, char **argv)
{
	try {
		QPainterPath line;
		line.moveTo(0, 0);
		line.lineTo(100, 0);
		check(QLineF(pathPrefix(line, 40).currentPosition(), QPointF(40, 0)).length() < .001,
		      "path prefix clips line at requested length");
		QPainterPath curve;
		curve.moveTo(0, 0);
		curve.cubicTo(20, 100, 80, 100, 100, 0);
		auto clipped = pathPrefix(curve, curve.length() / 2);
		check(QLineF(clipped.currentPosition(), QPointF(50, 75)).length() < .1,
		      "path prefix preserves cubic curve endpoint");
		check(clipped.elementCount() == 4, "path prefix preserves cubic representation");
		Canvas c;
		c.resize({320, 240});
		PaintStyle s;
		s.color = Qt::yellow;
		s.width = 10;
		s.tool = Tool::Highlighter;
		s.width = 40;
		s.opacity = .35;
		check(c.begin({30, 60}, s), "begin held cursor highlight");
		check(at(c, 30, 60).alpha() >= 88 && at(c, 30, 60).alpha() <= 90,
		      "highlight appears on press with requested opacity");
		for (int x = 31; x < 200; ++x)
			c.extend({double(x), 60});
		check(at(c, 199, 60).alpha() >= 88 && at(c, 199, 60).alpha() <= 90 && at(c, 30, 60).alpha() == 0,
		      "highlight follows cursor without trails or opacity buildup");
		c.breakSegment();
		check(c.snapshot(0).empty, "highlight hides outside video");
		c.extend({160, 100});
		check(at(c, 160, 100).alpha() > 0, "held highlight resumes on re-entry");
		c.finish();
		check(c.snapshot(0).empty && !c.historyAvailable().first,
		      "release removes highlight without undo entry");
		c.begin({30, 60}, s);
		c.cancel();
		check(c.snapshot(0).empty, "cancel removes highlight");
		PaintStyle ink;
		ink.tool = Tool::RectangleFill;
		ink.color = Qt::red;
		draw(c, ink, {5, 5}, {20, 20});
		draw(c, ink, {200, 150}, {220, 170});
		c.undo();
		const auto artwork = c.snapshot(0).image;
		const auto history = c.historyAvailable();
		c.begin({10, 10}, s);
		c.extend({150, 100});
		c.finish();
		check(c.snapshot(0).image == artwork && c.historyAvailable() == history,
		      "release preserves existing artwork and redo history");
		c.redo();
		check(at(c, 210, 160).red() == 255, "redo remains usable after highlight");
		c.clear();
		s = {};
		s.tool = Tool::Arrow;
		s.color = Qt::yellow;
		s.width = 8;
		s.opacity = .5;
		draw(c, s, {30, 60}, {210, 60});
		check(at(c, 100, 60).alpha() >= 126 && at(c, 100, 60).alpha() <= 128,
		      "arrow opacity applies once across outline and fill");
		check(at(c, 190, 67).alpha() > 0 && at(c, 50, 75).alpha() == 0, "arrowhead at destination only");
		c.clear();
		draw(c, s, {210, 60}, {30, 60});
		check(at(c, 50, 67).alpha() > 0 && at(c, 190, 75).alpha() == 0, "reverse arrow points to destination");
		c.clear();
		s.tool = Tool::CurvedArrow;
		s.opacity = 1;
		s.curvature = 80;
		draw(c, s, {30, 80}, {210, 80});
		check(at(c, 120, 152).alpha() > 0 && at(c, 120, 80).alpha() == 0, "curved route follows positive bend");
		c.clear();
		s.curvature = -50;
		draw(c, s, {30, 100}, {210, 100});
		check(at(c, 120, 55).alpha() > 0, "negative bend flips arc");
		c.clear();
		s.tool = Tool::DashedRoute;
		s.width = 8;
		s.contrastOutline = false;
		draw(c, s, {20, 50}, {260, 50});
		int filled = 0, empty = 0;
		for (int x = 30; x < 180; ++x) {
			if (at(c, x, 50).alpha() > 0)
				++filled;
			else
				++empty;
		}
		check(filled > 20 && empty > 10, "route contains dashes and gaps");
		c.clear();
		s.tool = Tool::PlayerRing;
		s.width = 6;
		draw(c, s, {40, 60}, {200, 120});
		check(at(c, 120, 60).alpha() > 200 && at(c, 120, 90).alpha() < 60,
		      "player ring has strong rim and translucent center");
		c.clear();
		s = {};
		s.tool = Tool::RectangleFill;
		s.color = Qt::red;
		draw(c, s, {5, 5}, {20, 20});
		const auto beforeSpotlight = c.snapshot(0).image;
		const auto beforeHistory = c.historyAvailable();
		s.tool = Tool::Spotlight;
		s.width = 100;
		s.opacity = .6;
		c.begin({90, 90}, s);
		check(at(c, 90, 90).alpha() == 0 && at(c, 250, 180).alpha() >= 152,
		      "spotlight appears immediately around cursor on press");
		check(at(c, 10, 10).red() == 255, "held spotlight preserves artwork on its layer");
		c.extend({230, 90});
		check(at(c, 230, 90).alpha() == 0 && at(c, 90, 90).alpha() >= 152,
		      "spotlight circle follows cursor without a drawn ellipse");
		c.breakSegment();
		check(c.snapshot(0).image == beforeSpotlight, "spotlight hides when pointer leaves video");
		c.extend({90, 90});
		check(at(c, 250, 180).alpha() >= 152, "held spotlight resumes on re-entry");
		c.finish();
		check(c.snapshot(0).image == beforeSpotlight && c.historyAvailable() == beforeHistory,
		      "release removes dimming and preserves artwork/history");
		c.begin({90, 90}, s);
		c.cancel();
		check(c.snapshot(0).image == beforeSpotlight, "cancel removes spotlight");
		c.begin({90, 90}, s);
		c.setLayerVisible(false);
		check(c.snapshot(0).empty, "changing layer visibility ends held spotlight");
		c.setLayerVisible(true);
		check(c.snapshot(0).image == beforeSpotlight, "showing layer does not resurrect spotlight");
		draw(c, ink, {200, 150}, {220, 170});
		c.undo();
		const auto redoHistory = c.historyAvailable();
		check(c.begin({90, 90}, s), "begin spotlight with pending redo");
		c.finish();
		check(c.historyAvailable() == redoHistory, "spotlight preserves redo history");
		c.clear();
		s = {};
		s.tool = Tool::NumberMarker;
		s.markerSize = 60;
		s.color = Qt::yellow;
		s.opacity = .5;
		draw(c, s, {60, 60}, {60, 60});
		check(c.nextMarker() == 2, "marker advances after placement");
		check(at(c, 40, 60).alpha() >= 126 && at(c, 40, 60).alpha() <= 128, "marker opacity");
		draw(c, s, {160, 60}, {160, 60});
		check(c.nextMarker() == 3, "second marker advances");
		c.undo();
		check(c.nextMarker() == 2 && at(c, 160, 60).alpha() == 0, "undo restores numbering and image");
		c.redo();
		check(c.nextMarker() == 3, "redo restores numbering");
		c.begin({250, 60}, s);
		c.cancel();
		check(c.nextMarker() == 3 && at(c, 250, 60).alpha() == 0, "cancel does not consume number");
		c.setNextMarker(99);
		draw(c, s, {250, 60}, {250, 60});
		check(c.nextMarker() == 1, "number wraps after 99");
		if (argc > 1) {
			Canvas demo;
			demo.resize({960, 540});
			PaintStyle a;
			a.tool = Tool::Brush;
			a.width = 64;
			a.opacity = .35;
			a.color = QColor("#ffe34b");
			draw(demo, a, {140, 350}, {390, 215});
			a = {};
			a.tool = Tool::Arrow;
			a.width = 9;
			a.color = QColor("#ffe34b");
			draw(demo, a, {190, 370}, {405, 275});
			a.tool = Tool::CurvedArrow;
			a.curvature = -65;
			draw(demo, a, {455, 340}, {735, 240});
			a.tool = Tool::DashedRoute;
			a.color = QColor("#69deff");
			demo.begin({530, 440}, a);
			demo.extend({660, 430});
			demo.extend({775, 355});
			demo.finish();
			a.tool = Tool::PlayerRing;
			a.width = 7;
			a.color = QColor("#ffe34b");
			draw(demo, a, {130, 390}, {235, 420});
			draw(demo, a, {705, 250}, {800, 280});
			a.tool = Tool::NumberMarker;
			a.markerSize = 52;
			a.color = QColor("#3976ef");
			draw(demo, a, {180, 365}, {180, 365});
			draw(demo, a, {750, 226}, {750, 226});
			QImage result(960, 540, QImage::Format_RGBA8888);
			result.fill(QColor("#1d6544"));
			QPainter p(&result);
			for (int x = 0; x < 960; x += 160)
				p.fillRect(x, 0, 80, 540, QColor("#206d49"));
			p.setPen(QPen(QColor("#b5d7b8"), 2));
			p.drawRect(30, 30, 900, 480);
			p.drawLine(480, 30, 480, 510);
			p.drawEllipse(QPointF(480, 270), 75, 75);
			p.drawRect(30, 150, 125, 240);
			p.drawRect(805, 150, 125, 240);
			p.drawImage(0, 0, demo.snapshot(0).image);
			p.end();
			check(result.save(QString::fromLocal8Bit(argv[1])), "save sports proof");
		}
		std::cout
			<< "PASS: 7 sports tools; held cursor highlight press/move/leave/release/history, arrow heads/bend/dashes, rings, held cursor spotlight and layer lifecycle, marker sequencing/undo/redo/cancel\n";
	} catch (const std::exception &e) {
		std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
	}
}
