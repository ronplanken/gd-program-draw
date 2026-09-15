// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas.hpp"
#include <iostream>
#include <stdexcept>
using namespace program_draw;
static void check(bool v, const char *message)
{
	if (!v)
		throw std::runtime_error(message);
}
static QColor pixel(Canvas &c, int x, int y)
{
	return c.snapshot(0).image.pixelColor(x, y);
}
static void drag(Canvas &c, Tool tool, QPointF a, QPointF b, double opacity = 1, QColor color = Qt::red)
{
	PaintStyle s;
	s.tool = tool;
	s.color = color;
	s.width = 6;
	s.opacity = opacity;
	check(c.begin(a, s), "begin");
	c.extend(b);
	c.finish();
}
int main()
{
	try {
		Canvas c;
		c.resize({200, 160});
		PaintStyle s;
		s.tool = Tool::None;
		check(!c.begin({20, 20}, s), "None is passive");
		for (auto t : {Tool::RectangleOutline, Tool::EllipseOutline}) {
			c.clear();
			drag(c, t, {20, 20}, {100, 100});
			check(pixel(c, 60, 60).alpha() == 0, "outline has no fill");
			check(pixel(c, 60, 20).alpha() > 0, "outline boundary");
		}
		for (auto t : {Tool::RectangleFill, Tool::EllipseFill}) {
			c.clear();
			drag(c, t, {100, 100}, {20, 20}, 0.5);
			check(pixel(c, 60, 60).alpha() >= 126 && pixel(c, 60, 60).alpha() <= 128,
			      "filled shapes obey opacity and reverse drag");
		}
		c.clear();
		drag(c, Tool::Line, {10, 30}, {150, 30});
		check(pixel(c, 70, 30).red() == 255, "line");
		c.clear();
		s = {};
		s.tool = Tool::RectangleFill;
		s.opacity = 0.5;
		c.begin({10, 10}, s);
		c.extend({130, 130});
		c.extend({50, 50});
		c.finish();
		check(pixel(c, 100, 100).alpha() == 0, "shape preview leaves no trail");
		check(pixel(c, 30, 30).alpha() <= 128, "shape preview opacity stable");
		for (auto t : {Tool::Pencil, Tool::Brush}) {
			c.clear();
			s = {};
			s.tool = t;
			s.opacity = 0.5;
			s.width = 10;
			c.begin({10, 30}, s);
			for (int x = 11; x <= 130; ++x)
				c.extend({double(x), 30});
			c.finish();
			check(pixel(c, 70, 30).alpha() >= 126 && pixel(c, 70, 30).alpha() <= 128,
			      "freehand opacity independent of samples");
		}
		c.clear();
		s = {};
		s.tool = Tool::RectangleFill;
		c.begin({20, 20}, s);
		c.extend({80, 100}, true);
		c.finish();
		check(pixel(c, 90, 90).alpha() > 0, "Shift makes square");
		c.clear();
		drag(c, Tool::RectangleFill, {10, 10}, {80, 80});
		auto before = c.snapshot(0);
		s = {};
		s.tool = Tool::SelectRectangle;
		c.begin({5, 5}, s);
		c.extend({90, 90});
		c.finish();
		check(c.snapshot(0).revision == before.revision, "selection guide does not alter broadcast pixels");
		check(!c.selectionImage().isNull(), "rectangle selection provides stamp image");
		c.begin({20, 20}, s);
		c.extend({110, 60});
		c.finish();
		check(pixel(c, 30, 30).alpha() == 0 && pixel(c, 120, 70).alpha() == 255,
		      "selection moves pixels and cuts original");
		c.undo();
		check(pixel(c, 30, 30).alpha() == 255 && pixel(c, 120, 70).alpha() == 0, "undo selection move");
		c.redo();
		check(pixel(c, 120, 70).alpha() == 255, "redo selection move");
		c.clear();
		drag(c, Tool::RectangleFill, {10, 10}, {100, 100});
		s.tool = Tool::SelectEllipse;
		c.begin({10, 10}, s);
		c.extend({100, 100});
		c.finish();
		auto selected = c.selectionImage();
		check(selected.pixelColor(0, 0).alpha() == 0 && selected.pixelColor(45, 45).alpha() > 0,
		      "ellipse mask");
		c.deleteSelection();
		check(pixel(c, 55, 55).alpha() == 0 && pixel(c, 12, 12).alpha() > 0,
		      "ellipse selection delete preserves corners");
		c.undo();
		check(pixel(c, 55, 55).alpha() > 0, "undo selection delete");
		c.clear();
		s = {};
		s.tool = Tool::Stamp;
		check(!c.begin({30, 30}, s), "missing stamp asset rejected");
		QImage asset(20, 10, QImage::Format_RGBA8888_Premultiplied);
		asset.fill(Qt::blue);
		s.asset = asset;
		s.imageWidth = 40;
		s.opacity = .5;
		c.begin({50, 50}, s);
		c.finish();
		c.begin({120, 50}, s);
		c.finish();
		check(pixel(c, 50, 50).blue() == 255 && pixel(c, 120, 50).alpha() >= 126, "repeat stamp with opacity");
		check(pixel(c, 50, 70).alpha() == 0, "stamp aspect ratio");
		c.clear();
		s.tool = Tool::Image;
		s.opacity = 1;
		c.begin({10, 10}, s);
		c.extend({110, 110});
		c.finish();
		check(pixel(c, 100, 50).blue() == 255 && pixel(c, 50, 90).alpha() == 0,
		      "image drag preserves aspect ratio");
		c.clear();
		drag(c, Tool::RectangleFill, {20, 20}, {100, 100});
		check(c.addLayer(), "add layer");
		c.renameLayer("Notes");
		drag(c, Tool::RectangleFill, {20, 20}, {100, 100}, 1, Qt::blue);
		check(pixel(c, 50, 50).blue() == 255, "top layer order");
		c.setLayerOpacity(.5);
		auto mixed = pixel(c, 50, 50);
		check(mixed.red() > 120 && mixed.blue() > 120, "layer opacity blends with lower layer");
		c.setLayerVisible(false);
		check(pixel(c, 50, 50).red() == 255, "hidden layer omitted");
		check(!c.begin({50, 50}, Qt::green, 10), "cannot paint hidden layer");
		c.setLayerVisible(true);
		c.setLayerOpacity(1);
		c.moveLayer(-1);
		check(pixel(c, 50, 50).red() == 255, "lower layer");
		c.moveLayer(1);
		check(pixel(c, 50, 50).blue() == 255, "raise layer");
		c.clearLayer();
		check(pixel(c, 50, 50).red() == 255, "clear only active layer");
		c.undo();
		check(pixel(c, 50, 50).blue() == 255, "undo clear layer");
		c.removeLayer();
		check(c.layers().size() == 1 && pixel(c, 50, 50).red() == 255, "remove active layer");
		c.undo();
		check(c.layers().size() == 2 && c.layers()[1].name == "Notes",
		      "undo layer removal restores artwork and name");
		c.clearAll();
		check(c.snapshot(0).empty, "user clear all");
		c.undo();
		check(pixel(c, 50, 50).blue() == 255, "undo user clear all");
		s = {};
		s.tool = Tool::RectangleFill;
		c.begin({120, 10}, s);
		c.extend({190, 100});
		c.cancel();
		check(pixel(c, 150, 50).alpha() == 0, "Escape cancels live draft");
		while (c.layers().size() < 8)
			check(c.addLayer(), "layer add until cap");
		check(!c.addLayer(), "layer memory cap");
		c.clear();
		c.undo();
		check(c.snapshot(0).empty, "lifecycle clear cannot resurrect annotations");
		Canvas locked;
		locked.resize({200, 160});
		drag(locked, Tool::RectangleFill, {10, 10}, {80, 80});
		locked.addLayer();
		locked.renameLayer("Protected");
		drag(locked, Tool::RectangleFill, {100, 10}, {180, 80}, 1, Qt::blue);
		drag(locked, Tool::SelectRectangle, {100, 10}, {180, 80});
		locked.setLayerLocked(true);
		const auto protectedImage = locked.snapshot(0).image;
		check(locked.selection().rect.isEmpty(), "locking clears an existing selection");
		for (int tool = int(Tool::Pencil); tool <= int(Tool::NumberMarker); ++tool) {
			PaintStyle paint;
			paint.tool = Tool(tool);
			paint.asset = asset;
			check(!locked.begin({120, 40}, paint), "locked layer rejects every drawing and selection tool");
		}
		locked.deleteSelection();
		locked.clearLayer();
		locked.clearSpotlight();
		locked.renameLayer("Changed");
		locked.setLayerOpacity(.2);
		locked.moveLayerTo(0);
		check(!locked.removeLayer(), "cannot remove a locked layer");
		check(locked.snapshot(0).image == protectedImage && locked.layers()[1].name == "Protected" &&
			      locked.layers()[1].opacity == 1 && locked.activeLayer() == 1,
		      "locked layer rejects content, metadata and order changes");
		locked.undo();
		check(!locked.layers()[1].locked, "blocked edits create no undo entries; undo unlocks");
		locked.redo();
		check(locked.layers()[1].locked, "redo restores lock");
		locked.setLayerVisible(false);
		check(pixel(locked, 120, 40).alpha() == 0, "eye can hide a locked layer");
		locked.setLayerVisible(true);
		locked.clearAll();
		check(pixel(locked, 40, 40).alpha() == 0 && pixel(locked, 120, 40).blue() == 255,
		      "clear all preserves locked artwork while clearing other layers");
		locked.undo();
		check(locked.snapshot(0).image == protectedImage && locked.layers()[1].locked,
		      "undo clear all preserves locked state and restores other artwork");
		locked.clear();
		locked.undo();
		check(pixel(locked, 40, 40).alpha() == 0 && pixel(locked, 120, 40).blue() == 255,
		      "scene clear preserves locked artwork and discards old history");
		locked.setLayerLocked(false);
		locked.clearLayer();
		check(locked.snapshot(0).empty, "unlock allows artwork to be cleared");

		Canvas order;
		order.resize({200, 160});
		for (int i = 0; i < 4; ++i) {
			if (i)
				order.addLayer();
			order.renameLayer(QString::number(i));
		}
		order.selectLayer(0);
		order.moveLayerTo(3);
		check(order.layers()[0].name == "1" && order.layers()[2].name == "3" && order.layers()[3].name == "0" &&
			      order.activeLayer() == 3,
		      "nonadjacent drag inserts layer and preserves intermediate order");
		order.undo();
		check(order.layers()[0].name == "0" && order.activeLayer() == 0,
		      "one undo restores drag order and selection");
		order.redo();
		check(order.layers()[3].name == "0" && order.activeLayer() == 3,
		      "redo restores drag order and selection");
		order.moveLayerTo(0);
		check(order.layers()[0].name == "0" && order.layers()[3].name == "3",
		      "reverse drag restores original order");
		order.moveLayerTo(-1);
		order.moveLayerTo(9);
		check(order.activeLayer() == 0, "out-of-range drops are ignored");
		std::cout
			<< "PASS: all 12 tools, shape drafts, shift constraints, tool opacity, selection move/delete/masks, stamp/image, layers/order/visibility/opacity, undo/redo/cancel\n";
	} catch (const std::exception &e) {
		std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
	}
}
