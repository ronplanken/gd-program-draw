// SPDX-License-Identifier: GPL-2.0-or-later
// Tests the real libobs video pipeline, with a synthetic background and no user scenes/devices.
#include "ink-source.hpp"
#include <QCoreApplication>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <graphics/vec4.h>
using namespace program_draw;
struct Frames {
	std::mutex mutex;
	std::condition_variable changed;
	QImage latest;
	uint64_t count = 0;
	static void receive(void *data, video_data *frame)
	{
		auto &f = *static_cast<Frames *>(data);
		std::lock_guard lock(f.mutex);
		f.latest = QImage(frame->data[0], 320, 180, frame->linesize[0], QImage::Format_RGBA8888).copy();
		++f.count;
		f.changed.notify_all();
	}
	template<typename Predicate> QImage until(Predicate matches)
	{
		std::unique_lock lock(mutex);
		if (!changed.wait_for(lock, std::chrono::seconds(5),
				      [&] { return !latest.isNull() && matches(latest); })) {
			std::cerr << "Frames received: " << count;
			if (!latest.isNull())
				std::cerr << "; sample(80,60): " << latest.pixelColor(80, 60).name().toStdString()
					  << "; inside: " << latest.pixelColor(90, 90).name().toStdString()
					  << "; outside: " << latest.pixelColor(250, 140).name().toStdString();
			std::cerr << '\n';
			throw std::runtime_error("Timed out waiting for expected composited video pixels");
		}
		return latest;
	}
};
static void *bgCreate(obs_data_t *, obs_source_t *)
{
	return reinterpret_cast<void *>(1);
}
static void bgDestroy(void *) {}
static void bgRender(void *, gs_effect_t *)
{
	vec4 color;
	vec4_set(&color, 0.02f, 0.03f, 0.08f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &color, 1.0f, 0);
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc != 3) {
		std::cerr << "Usage: render-tests /Applications/OBS.app /path/to/test-frame.png\n";
		return 2;
	}
	std::string frameworks = std::string(argv[1]) + "/Contents/Frameworks";
	if (!obs_startup("en-US", nullptr, nullptr))
		return 3;
	obs_video_info video{};
	std::string graphics = frameworks + "/libobs-opengl.dylib";
	video.graphics_module = graphics.c_str();
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.output_width = 320;
	video.base_height = video.output_height = 180;
	// libobs's unconverted render/staging textures are BGRA. Request RGBA
	// on the raw callback so libobs's scaler performs the channel conversion.
	video.output_format = VIDEO_FORMAT_BGRA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	video.scale_type = OBS_SCALE_DISABLE;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
		obs_shutdown();
		return 4;
	}
	obs_source_info bg{};
	bg.id = "program_draw_test_background";
	bg.type = OBS_SOURCE_TYPE_INPUT;
	bg.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	bg.get_name = [](void *) {
		return "Synthetic test background";
	};
	bg.create = bgCreate;
	bg.destroy = bgDestroy;
	bg.video_render = bgRender;
	bg.get_width = [](void *) -> uint32_t {
		return 320;
	};
	bg.get_height = [](void *) -> uint32_t {
		return 180;
	};
	obs_register_source(&bg);
	auto c = std::make_shared<Canvas>();
	c->resize({320, 180});
	registerInkSource(c);
	auto *background = obs_source_create_private(bg.id, "test background", nullptr);
	auto *ink = obs_source_create_private(SourceId, "test ink", nullptr);
	obs_set_output_source(0, background);
	obs_set_output_source(63, ink);
	Frames frames;
	video_scale_info conversion{};
	conversion.format = VIDEO_FORMAT_RGBA;
	conversion.width = 320;
	conversion.height = 180;
	conversion.colorspace = VIDEO_CS_709;
	conversion.range = VIDEO_RANGE_FULL;
	obs_add_raw_video_callback(&conversion, Frames::receive, &frames);
	int result = 0;
	try {
		auto baseline = frames.until([](const QImage &im) { return im.pixelColor(80, 60).red() < 150; });
		std::cout << "Background frame received\n";
		c->begin({30, 60}, Qt::red, 16);
		c->extend({150, 60});
		c->finish();
		auto composed = frames.until([&baseline](const QImage &im) {
			return im.pixelColor(80, 60).red() > 230 && im.pixelColor(80, 60).green() < 30 &&
			       std::abs(im.pixelColor(80, 110).blue() - baseline.pixelColor(80, 110).blue()) <= 2;
		});
		if (!composed.save(QString::fromLocal8Bit(argv[2])))
			throw std::runtime_error("Saving frame failed");
		std::cout << "Red stroke received\n";
		c->begin({30, 120}, Qt::green, 12);
		c->extend({150, 120});
		c->finish();
		frames.until([](const QImage &im) { return im.pixelColor(80, 120).green() > 230; });
		c->undo();
		frames.until([](const QImage &im) {
			return im.pixelColor(80, 120).green() < 150 && im.pixelColor(80, 60).red() > 230;
		});
		c->clear();
		frames.until([](const QImage &im) { return im.pixelColor(80, 60).red() < 150; });
		PaintStyle shape;
		shape.tool = Tool::RectangleFill;
		shape.color = Qt::red;
		c->begin({30, 40}, shape);
		c->extend({150, 90});
		c->finish();
		frames.until([](const QImage &im) { return im.pixelColor(80, 60).red() > 230; });
		c->addLayer();
		shape.color = Qt::blue;
		c->begin({30, 40}, shape);
		c->extend({150, 90});
		c->finish();
		frames.until([](const QImage &im) { return im.pixelColor(80, 60).blue() > 230; });
		c->setLayerOpacity(.5);
		auto layered = frames.until([](const QImage &im) {
			auto p = im.pixelColor(80, 60);
			return p.red() > 100 && p.red() < 160 && p.blue() > 100 && p.blue() < 160;
		});
		layered.save(QString::fromLocal8Bit(argv[2]));
		c->setLayerVisible(false);
		frames.until([](const QImage &im) {
			return im.pixelColor(80, 60).red() > 230 && im.pixelColor(80, 60).blue() < 30;
		});
		c->clear();
		frames.until([](const QImage &im) { return im.pixelColor(80, 60).red() < 150; });
		c->setLayerVisible(true);
		c->setLayerOpacity(1);
		shape.tool = Tool::Arrow;
		shape.color = Qt::yellow;
		shape.width = 8;
		c->begin({30, 60}, shape);
		c->extend({200, 60});
		c->finish();
		frames.until([](const QImage &im) {
			auto p = im.pixelColor(80, 60);
			return p.red() > 230 && p.green() > 230 && p.blue() < 40;
		});
		std::cout << "Arrow output passed\n";
		c->clear();
		auto clean = frames.until([](const QImage &im) { return im.pixelColor(80, 60).red() < 150; });
		const int backgroundBlue = clean.pixelColor(250, 140).blue();
		std::cout << "Background blue=" << backgroundBlue << "\n";
		shape.tool = Tool::Spotlight;
		shape.opacity = .7;
		shape.width = 120;
		c->begin({90, 90}, shape);
		frames.until([backgroundBlue](const QImage &im) {
			return im.pixelColor(250, 140).blue() < backgroundBlue - 2 &&
			       std::abs(im.pixelColor(90, 90).blue() - backgroundBlue) <= 2;
		});
		std::cout << "Spotlight output passed\n";
		c->extend({250, 90});
		frames.until([backgroundBlue](const QImage &im) {
			return im.pixelColor(90, 90).blue() < backgroundBlue - 2 &&
			       std::abs(im.pixelColor(250, 90).blue() - backgroundBlue) <= 2;
		});
		c->finish();
		frames.until([backgroundBlue](const QImage &im) {
			return std::abs(im.pixelColor(250, 140).blue() - backgroundBlue) <= 2;
		});
		shape.tool = Tool::Highlighter;
		shape.color = Qt::yellow;
		shape.width = 40;
		shape.opacity = .5;
		c->begin({60, 60}, shape);
		frames.until([](const QImage &im) {
			auto p = im.pixelColor(60, 60);
			return p.red() > 120 && p.green() > 120;
		});
		c->extend({200, 60});
		frames.until([&baseline](const QImage &im) {
			return im.pixelColor(200, 60).red() > 120 &&
			       std::abs(im.pixelColor(60, 60).red() - baseline.pixelColor(60, 60).red()) <= 2;
		});
		c->finish();
		frames.until([&baseline](const QImage &im) {
			return std::abs(im.pixelColor(200, 60).red() - baseline.pixelColor(200, 60).red()) <= 2;
		});
		std::cout << "Held cursor highlight appears, follows and disappears in real video output\n";
		std::cout
			<< "PASS: real libobs video verifies ink, shapes, layers, arrows, spotlight dimming/removal, background, undo and clear\n";
	} catch (const std::exception &e) {
		std::cerr << "FAIL: " << e.what() << '\n';
		result = 1;
	}
	obs_remove_raw_video_callback(Frames::receive, &frames);
	obs_set_output_source(63, nullptr);
	obs_set_output_source(0, nullptr);
	obs_source_release(ink);
	obs_source_release(background);
	obs_shutdown();
	return result;
}
