// SPDX-License-Identifier: GPL-2.0-or-later
#include "canvas.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
using namespace program_draw;
static void require(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}
static bool near(double a, double b)
{
	return std::abs(a - b) < 0.01;
}
int main()
{
	try {
		// Physical 10px border at 1x and Retina; verify both aspect-fit branches.
		for (double dpr : {1.0, 1.5, 2.0}) {
			for (QSize monitor : {QSize(800, 600), QSize(900, 300), QSize(401, 711)}) {
				for (QSize video : {QSize(1920, 1080), QSize(1080, 1920)}) {
					auto r = programVideoRect(monitor, dpr, video);
					auto p = mapToCanvas(r.center(), monitor, dpr, video);
					require(p && near(p->x(), video.width() / 2.0) &&
							near(p->y(), video.height() / 2.0),
						"center mapping");
					require(!mapToCanvas(QPointF(0, 0), monitor, dpr, video), "letterbox excluded");
					auto edge = mapToCanvas(r.topLeft(), monitor, dpr, video);
					require(edge && near(edge->x(), 0) && near(edge->y(), 0), "origin mapping");
				}
			}
		}
		auto retina = programVideoRect(QSize(960, 540), 2, QSize(1920, 1080));
		require(near(retina.y(), 5), "Retina margin must be 5 logical pixels, not 10");
		require(programVideoRect(QSize(10, 10), 1, QSize(1920, 1080)).isEmpty(), "tiny widget");

		Canvas c;
		c.resize({200, 100});
		require(c.begin({20, 20}, Qt::red, 8), "begin stroke");
		c.extend({100, 20});
		c.finish();
		auto red = c.snapshot(0);
		require(!red.empty && red.image.pixelColor(50, 20).red() == 255, "red stroke pixels");
		require(red.image.pixelColor(50, 50).alpha() == 0, "transparent background");
		require(c.snapshot(red.revision).image.isNull(), "unchanged frames avoid texture upload");
		c.begin({20, 70}, Qt::blue, 8);
		c.extend({100, 70});
		c.finish();
		require(red.image.pixelColor(50, 70).alpha() == 0, "render snapshots immutable while painting");
		c.undo();
		auto undo = c.snapshot(0);
		require(undo.image.pixelColor(50, 70).alpha() == 0 && undo.image.pixelColor(50, 20).alpha() == 255,
			"undo last stroke only");
		c.clear();
		require(c.snapshot(0).empty && c.snapshot(0).image.pixelColor(50, 20).alpha() == 0, "clear output");
		c.begin({10, 10}, Qt::red, 4);
		c.breakSegment();
		c.extend({190, 90});
		c.finish();
		require(c.snapshot(0).image.pixelColor(100, 50).alpha() == 0, "no diagonal bridge after leaving video");
		c.resize({100, 200});
		require(c.snapshot(0).empty && c.snapshot(0).image.size() == QSize(100, 200),
			"resolution change clears stale ink");
		std::atomic<bool> reading{true};
		std::thread renderer([&] {
			while (reading) {
				auto s = c.snapshot(0);
				(void)s.image.constBits();
			}
		});
		for (int i = 0; i < 100; ++i) {
			c.begin({10, 10}, Qt::red, 5);
			c.extend({50, 50});
			c.finish();
			c.clear();
		}
		reading = false;
		renderer.join();
		std::cout
			<< "PASS: 18 coordinate cases, Retina margins, transparency, snapshot isolation, undo/clear, segment breaks, resize, concurrent snapshots\n";
	} catch (const std::exception &e) {
		std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
	}
}
